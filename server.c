#include <sys/socket.h>
#include <errno.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h> // write getopt
#include <string.h> // memset
#include <stdlib.h> // atoi
#include <stdbool.h> // true, false
#include <ctype.h> // isalnum
#include <getopt.h> //optarg
#include <pthread.h>
#include "list.h"

#define BUFFER_SIZE 1600
#define ARG_SIZE 100 

struct httpObject {
    char method[ARG_SIZE];         // PUT, HEAD, GET
    int method_num;
    char filename[ARG_SIZE];      // what is the file we are worried about
    char httpversion[9];    // HTTP/1.1
    ssize_t content_length; // example: 13
    int status_code;
    char buffer[BUFFER_SIZE];
    char response[300];
    ssize_t body_data_length;
    size_t success_requests;
    size_t unsuccess_requests;
};

typedef struct threadArg_t{
    //int        client_sockd; //for dispatcher
    // int*       clients;      //for thread [i] 
    // int*       thread_id;
    pthread_t* threads;      //list of worker threads
    List*       client_queue;
    pthread_mutex_t queue_mutex;
    pthread_cond_t wake_threads;
    pthread_mutex_t offset_mutex;
    size_t log_offset;
    size_t total_requests;
    size_t failed_requests;
    pthread_mutex_t total_requests_lock;
    int log_fd;
} ThreadArg;


int checkDirectory(struct httpObject* message);
int openRDONLY(struct httpObject* message);
int openWRONLY(struct httpObject* message);
int openLog(char* filename);
int readSend(int fd, int client, struct httpObject* message);
int recvWrite(int fd, int client, struct httpObject* message);
ssize_t recv_full(ssize_t fd, struct httpObject* message);
int read_http_request(ssize_t client_sockd, struct httpObject* message, int log_fd);
int process_request(ssize_t client_sockd, struct httpObject* message, void* threadArgs);
void construct_http_response(struct httpObject* message);
void* serveClient(void* threadArgs);

void log_success(struct httpObject* message, void* threadArgs);
void log_failure(struct httpObject* message, void* threadArgs);


int main(int argc, char** argv){
    ThreadArg *threadArgs = malloc(sizeof(ThreadArg));
    threadArgs->log_offset = 0;
    pthread_mutex_init(&threadArgs->total_requests_lock, NULL);
    pthread_mutex_init(&threadArgs->queue_mutex, NULL);
    pthread_mutex_init(&threadArgs->offset_mutex, NULL);
    pthread_cond_init(&threadArgs->wake_threads, NULL);
    threadArgs->failed_requests = 0;
    threadArgs->total_requests = 0;
    List queue = newList();
    threadArgs->client_queue = &queue;
    int num_threads = 4;
    char* log_fileName = NULL;
    int c;
    int er;
    while ((c = getopt(argc, argv, "N:l:")) != -1){
        switch (c) {
            case 'N':
                er = sscanf(optarg,"%d",&num_threads);
                if(er != 1){
                    return EXIT_FAILURE;
                }
                break;
            case 'l':
                log_fileName = optarg;
                threadArgs->log_fd = openLog(log_fileName);
                if(threadArgs->log_fd < 0){
                    //printf("error opening logfile");
                    return EXIT_FAILURE;
                }
                break;
            default:
                //printf("ERROR: invalid args");
                return EXIT_FAILURE;
         }
    }
    char* port = argv[optind];
    if(port == NULL){
        return EXIT_FAILURE;
    }
    for(size_t i = 0; i < strlen(port); i++){
        if (!isdigit(port[i])){
            return EXIT_FAILURE;
        }
    }
    
    pthread_t threads[num_threads];
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(port));
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t addrlen = sizeof(server_addr);
    int server_sockd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sockd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    int enable = 1;
    int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);
    ret = listen(server_sockd, 5); // 5 should be enough, if not use SOMAXCONN
    if (ret < 0) {
        return EXIT_FAILURE;
    }
    struct sockaddr client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    //printf("listening on port : %s\n",port);
    for (int idx = 0; idx < num_threads; ++idx) { 
        int err = pthread_create(&threads[idx], NULL, serveClient, (void*)threadArgs);
        if (err == -1){
            //printf("Thread error");
            return -1;
        }
    }
    while(true){
        int client_sockd = accept(server_sockd, &client_addr, &client_addrlen);
        if (client_sockd == -1){
            warn("%s","accept");
            continue;
        }
        pthread_mutex_lock(&threadArgs->queue_mutex);
        enQueue(queue, client_sockd);
        pthread_cond_signal(&threadArgs->wake_threads);
        pthread_mutex_unlock(&threadArgs->queue_mutex);
        //printf("connection recieved\n");
    }
    return 0;
}
// Do I need a dispatch t--hread? 
// Handle case where all threads are busy to handle something on the queue
// But then no more clients connect to trigger a broadcast
void* serveClient(void* threadArgs){    
    ThreadArg *sharedMemory = (ThreadArg*) threadArgs; 
    while(true){
         pthread_mutex_lock(&sharedMemory->queue_mutex);
         while(isEmpty(*sharedMemory->client_queue)){
            //printf("\033[0;33m[-] thread %ld going to Sleep\n\033[0m", pthread_self());
            pthread_cond_wait(&sharedMemory->wake_threads, &sharedMemory->queue_mutex);
            //printf("\033[1;32m[-] thread %ld Signaled to WAKE UP\033[0m", pthread_self());
        }
        int client_sockd = deQueue(*sharedMemory->client_queue);
        pthread_mutex_unlock(&sharedMemory->queue_mutex);
        //printf("\nThread %ld, is now serving client : %d\n",pthread_self(), client_sockd);
        struct httpObject message;
        message.content_length = 0;
        message.status_code    = 200;
        int err = read_http_request(client_sockd, &message, sharedMemory->log_fd);
        if (err == -1){
            pthread_mutex_lock(&sharedMemory->total_requests_lock);
            sharedMemory->failed_requests++;
            sharedMemory->total_requests++;
            pthread_mutex_unlock(&sharedMemory->total_requests_lock);

            construct_http_response(&message);
            send(client_sockd, message.response, strlen(message.response),0);
            
            if (sharedMemory->log_fd > 0)
                log_failure(&message, sharedMemory);
            close(client_sockd);
            continue;
        }

        //** implement file locks **/
        err = process_request(client_sockd, &message, sharedMemory);
        if (err == -1){
            pthread_mutex_lock(&sharedMemory->total_requests_lock);
            sharedMemory->failed_requests++;
            sharedMemory->total_requests++;
            pthread_mutex_unlock(&sharedMemory->total_requests_lock);

            construct_http_response(&message);
            send(client_sockd, message.response, strlen(message.response),0);

            if (sharedMemory->log_fd > 0) 
                log_failure(&message, sharedMemory);
            close(client_sockd);
            continue;
        }
        /*
            1) reserver N number of bytes to write to the file //SYNCHRONOUS (NEEDS LOCK)
            2) write to file with pwrite() using the offset    //ASYNCHROUNOUS(PARALLELE)
        */ 
        pthread_mutex_lock(&sharedMemory->total_requests_lock);
        sharedMemory->total_requests++;
        pthread_mutex_unlock(&sharedMemory->total_requests_lock);

        if(sharedMemory->log_fd > 0 )
            log_success(&message, sharedMemory);
        close(client_sockd);
        //printf("Response Sent\n");
        //printf("\n\n");
   }
}

int openRDONLY(struct httpObject* message){
    int fd = open(message->filename, O_RDONLY);
    if(fd < 0){
        warn("%s",message->filename);
        if(errno == EACCES){
            message->status_code = 403;
            return -1;
        }else if(errno = ENONET){
            message->status_code = 404;
            warn("%s",message->filename);
            return -1;
        }else{
            message->status_code = 404;
            warn("%s",message->filename);
            return -1;
        }
    }
    return fd;
}


int openLog(char* filename){
    int fd = open(filename, O_RDWR |O_CREAT | O_TRUNC, 0644 );
    if(fd < 0){
      if(errno == EACCES){
        return -1;
      }else{
          return -1;
      }
    }
    return fd; 
}

int openWRONLY(struct httpObject* message){
    int fd = open(message->filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0){
      if(errno == EACCES){
        warn("%s",message->filename);
        message->status_code = 403;
        return -1;
      }else{
          message->status_code = 404;
          warn("%s",message->filename);
          return -1;
      }
    }
    return fd; 
}
// DESCRIPTION: repeatedly reads/writes file descriptor to stdout
// PRECONDITION: fileName has been opened()
// POSTCONDITION: returns -1 if error else returns 0
int recvWrite(int fd, int client, struct httpObject* message){
    u_int8_t buffer[BUFFER_SIZE];
    ssize_t bytesRead, bytesWrote, bytesTotal;
    bytesRead = 0;
    bytesWrote = 0;
    bytesTotal = 0;
    char* header_end = strstr(message->buffer,"\r\n\r\n") + 4;
    if(message->body_data_length > 0 && message->content_length > 0){
        write(fd,header_end,message->body_data_length);
        bytesTotal = message->body_data_length;
    }
    while (bytesTotal < (ssize_t)message->content_length){
        bytesRead = recv(client, buffer, BUFFER_SIZE-1,0);
        if(bytesRead < 0){
            if (bytesRead == 0)
            message->status_code = 400;
            warn("%s",message->filename);
            return -1;
        }
        bytesTotal += bytesRead;
        bytesWrote += write(fd, buffer, bytesRead);
        if(bytesWrote < 0){
            message->status_code = 400;
            warn("%s",message->filename);
            return -1;
        }
    }
    //printf("Bytes read %ld, Content Length %ld", bytesRead, message->content_length);
    close(fd);
    return 0;
}
// DESCRIPTION: repeated read / sends data for GET requests 
// PRECONDITION: fileName has been opened()
// POSTCONDITION: returns -1 if error sets message status else returns 
int readSend(int fd, int client, struct httpObject* message){
   u_int8_t buffer[BUFFER_SIZE];
   ssize_t bytesRead, bytesWrote;
   //printf("starting");
   while((bytesRead = read(fd, buffer, BUFFER_SIZE-1)) != 0){
      if(bytesRead < 0){
        message->status_code = 400;
        warn("%s",message->filename);
        return -1;
      }
      bytesWrote = send(client, buffer, bytesRead,0);
      if(bytesWrote < 0){
        message->status_code = 400;
        //printf("ERRRRRRRRRRRR");
        warn("%s",message->filename);
        return -1;
      }
   }
   close(fd);
   return 0;
}

// DESCRIPTION: repeated recvs until full header is read in
// PRECONDITION: socket connection has been set
// POSTCONDITION: returns -/1 if error sets message status, records any body data read 
ssize_t recv_full(ssize_t fd, struct httpObject* message){
    ssize_t total = 0;
    char* header_end;
    while(total < BUFFER_SIZE){
        ssize_t ret = recv(fd, message->buffer+total, BUFFER_SIZE-total, 0);
        if (ret < 0){
            if (errno == EAGAIN) {
                continue;
            }
            return ret;
        }else if (ret == 0){
            return total;
        } else {
            total += ret;
        }
        message->buffer[total] = '\0';
        header_end = strstr(message->buffer,"\r\n\r\n");
        if(header_end != NULL){
            ssize_t bodyStart = (&header_end[0] - &message->buffer[0]) + 4;
            ssize_t len = total - bodyStart; 
            if(len > 0){
                message->body_data_length = len;
            }else{
                message->body_data_length = len;
            }
            return total;
        }
    }
    return total;
}

// DESCRIPTION: parses through http request
// PRECONDITION: full header has been read 
// POSTCONDITION: returns -1 if error sets message status, else sets message data accordingly 
int read_http_request(ssize_t client_sockd, struct httpObject* message, int log_fd) {
    char* buff = message->buffer;
    ssize_t bytes = recv_full(client_sockd, message);
    if(bytes == -1){
        warn("%s","recv");
        message->status_code = 400;
        return -1;
    }
    //printf("\n");
    //printf("[+] received %ld bytes from client[+]\n", bytes);
    // char command[ARG_SIZE];a
    // char pathName[ARG_SIZE]; 
    // char version[ARG_SIZE];
    char* command = message->method;
    char pathName [ARG_SIZE];
    //char fileName [ARG_SIZE];
    char* version  = message->httpversion;
    int numScanned =sscanf(buff,"%s %s %s", command, pathName, version);
    if(pathName[0] == '/'){
        strcpy(message->filename, pathName+1);
    }else{
        strcpy(message->filename, pathName);
        //printf("    ERROR: filename does not contain a /");
        message->status_code = 400;
        return -1;
    }
    if(numScanned != 3){
        //printf("    ERROR: first line does not contain 3 strings");
        message->status_code = 400;
        return -1;
    }
    if(strcmp(command, "GET") == 0){
        message->method_num = 0;
    }else if(strcmp(command,"PUT") == 0){
        message->method_num = 1;
    }else if(strcmp(command, "HEAD") == 0){
        message->method_num = 2;
    }else{
        //printf("    ERROR: unsupported request");
        message->status_code = 400;
        return -1;
    }
    if (strcmp(command, "PUT") == 0|| strcmp(command, "put") == 0){
        char* contLengthPtr = strstr(buff,"\r\nContent-Length:");
        if (contLengthPtr == NULL){
            //printf("    ERROR: no end of header");
            message->status_code = 400;
            return -1;
        }
        int ret = sscanf(contLengthPtr, "%*s %ld", &message->content_length);
        if(ret != 1){
            //printf("    ERROR: no Content-Length field");
            message->status_code = 400;
            return -1;
        }
    }else{
        message->content_length = 0;
    }
    if(message->content_length < 0){
            //printf("    ERROR: negative Content-Length field\n");
            message->status_code = 400;
            return -1;
    }
    if(strcmp(version,"HTTP/1.1") != 0){
        message->status_code = 400;
        return -1;
    }
        if(strlen(message->filename) == 0){
        //printf("    ERROR: empty file name");
        message->status_code = 400;
        return -1;
    }
    for(size_t i = 0; i < strlen(message->filename); i++){
        if (!isalnum(message->filename[i]) && message->filename[i] != '-' && message->filename[i] != '_'){
            //printf("    ERROR: filename contains invalid characters");
            message->status_code = 400;
            return -1;
        }
    }
    if(strlen(message->filename) > 27){
        //printf("    ERROR: pathname is more than 27 characters");
        message->status_code = 400;
        return -1;
    }

    if(message->method == NULL || message->filename == NULL || message->httpversion == NULL){
        //printf("    ERROR NULL command | filename | version");
        message->status_code = 400;
        return -1;
    }
    if(strcmp(message->filename,"healthcheck") == 0){
        if(message->method_num == 0){
            if(log_fd > 0){
                message->method_num = 3;
            }else{
                message->status_code = 404;
                return -1;
            }
        }else{
            message->status_code = 403;
            return -1;
        }
    }
    // printf("Parsed request : %s , %d\n", message->method, message->method_num);
    // printf("Filename       = %s\n", message->filename);
    // printf("HTTP Version   = %s\n", message->httpversion);
    // printf("Content-Length = %ld\n", message->content_length);
    // printf("request code  = %d\n", message->method_num);
    return 0;
}
// DESCRIPTION: Executes request bassed of message
// PRECONDITION: message conents have beens set
// POSTCONDITION: returns -1 if error sets message status executes request
int process_request(ssize_t client_sockd, struct httpObject* message, void* threadArgs) {
    //printf("Processing Request\n");
    int fd;
    int errCheck;
    struct stat fileStat;
    ThreadArg *sharedMemory = (ThreadArg*) threadArgs;  
    switch (message->method_num){
        case 0:   //GET
            //printf("    Processing GET");     
            errCheck = stat(message->filename, &fileStat);
            if (errCheck < 0){
                if(errno == EACCES){
                    message->status_code = 404;
                    return -1;
                }else {
                    message->status_code = 404;
                    return -1;
                }
            }
             if((fileStat.st_mode & S_IRUSR) == S_IRUSR){
                 //printf("\n     File has read permissions\n");
            }else{
                //printf("    No permisisons");
                message->status_code = 403;
                return -1;
            }
            if( (fileStat.st_mode & __S_IFDIR) == __S_IFDIR){
                //printf("File is a directory");
                message->status_code = 403;
                return -1;
            }
            fd = openRDONLY(message);
            if (fd < 0){
                return -1;
            }
            message->content_length = fileStat.st_size;
            construct_http_response(message);
            //printf("Sending HTTP response header of size %ld\n",strlen(message->response));
            send(client_sockd, message->response, strlen(message->response),0);
            errCheck = readSend(fd, client_sockd, message);
            if(errCheck < 0){
                return -1;
            }
            break;
        case 1:   //PUT
            //printf("    Processing PUT\n");
            errCheck = stat(message->filename, &fileStat);
            if (errCheck >= 0){
                if((fileStat.st_mode & S_IWUSR) == S_IWUSR){
                    //printf("\n     File has write permissions\n");
                }else{
                    //printf("    No permisisons");
                    message->status_code = 403;
                    return -1;
                }
                if( (fileStat.st_mode & __S_IFDIR) == __S_IFDIR){
                    //printf("File is a directory");
                    message->status_code = 403;
                    return -1;
                }
            }
            fd = openWRONLY(message);
            if (fd < 0){
                return -1;
            }
            errCheck = recvWrite(fd, client_sockd, message);
            if (errCheck < 0){
                return -1;
            }
            message->status_code = 201;
            construct_http_response(message);
            send(client_sockd, message->response, strlen(message->response),0);
            //printf("messagesent");
            break;
        case 2:   //HEAD
            //printf("    Processing HEAD");
            errCheck = stat(message->filename, &fileStat);
            if (errCheck < 0){
                if(errno == EACCES){
                    message->status_code = 404;
                    return -1;
                }else {
                    message->status_code = 404;
                    return -1;
                }
            }
             if((fileStat.st_mode & S_IRUSR) == S_IRUSR){
                 //printf("\n     File has read permissions\n");
            }else{
                //printf("    No permisisons");
                message->status_code = 403;
                return -1;
            }
            if( (fileStat.st_mode & __S_IFDIR) == __S_IFDIR){
                //printf("File is a directory");
                message->status_code = 403;
                return -1;
            }
            fd = openRDONLY(message);
            if (fd < 0){
                return -1;
            }
            message->content_length = fileStat.st_size;
            construct_http_response(message);
            send(client_sockd, message->response, strlen(message->response),0);
            break;
        case 3: // HealthCheck
           
            pthread_mutex_lock(&sharedMemory->total_requests_lock);
            message->success_requests = sharedMemory->total_requests;
            message->unsuccess_requests = sharedMemory->failed_requests;
            pthread_mutex_unlock(&sharedMemory->total_requests_lock);
            construct_http_response(message);
            send(client_sockd, message->response, strlen(message->response),0);
            break;
        default:
            return -1;
    }
    return 0;
}


/*
    Generates message based of message->status_code
    brief 3. Construct some response based on the HTTP request you recieved
    200 OK
    201 Created
    400 Bad Request
    403 Forbidden
    404 Not Found – the case in GET and HEAD requests
    500 Internal Server Error
*/
void construct_http_response(struct httpObject* message) {
    //printf("Constructing Response\n");
    if(message->method_num == 3){
        char health_body[100];
        message->content_length = snprintf(health_body, 100, "%01ld\n%01ld", message->unsuccess_requests, message->success_requests);
        sprintf(message->response, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n%s",
                message->content_length,
                health_body);
        return;
    }
//HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\n54\n193
    switch (message->status_code){
        case 200: 
            sprintf(message->response, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n",
                message->content_length);
            break;
        case 201: 
            sprintf(message->response, "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
            break;
        case 400: 
            sprintf(message->response, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
            break;
        case 403: 
            sprintf(message->response, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
            break;
        case 404: 
            sprintf(message->response, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            break;
        case 500: 
            sprintf(message->response, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
            break;
        default:
            sprintf(message->response, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
    }
}


void log_success(struct httpObject* message, void* threadArgs){
    ThreadArg *sharedMemory = (ThreadArg*) threadArgs; 
    int fd;
    unsigned char buffer[BUFFER_SIZE];
    unsigned char hex_buffer[BUFFER_SIZE]; // converted data
    int bytesRead,bytesWrote, bytesTotal, hex_offset;
    bytesRead=bytesWrote= bytesTotal= hex_offset= 0;
    struct stat fileStat;
    size_t file_length;

    if(message->method_num == 3){
        fd = open("tmpHealthCheck", O_RDWR |O_CREAT | O_TRUNC, 0644 );
        char tmp[BUFFER_SIZE];
        int sz = sprintf(tmp, "%0ld\n%0ld", message->unsuccess_requests, message->success_requests);
        write(fd, tmp, sz);
        close(fd);
        fd = open("tmpHealthCheck",O_RDONLY);
        stat("tmpHealthCheck", &fileStat);
        file_length = fileStat.st_size;
        message->content_length = file_length;
    }else if(message->method_num == 2){
        stat(message->filename, &fileStat);
        file_length = fileStat.st_size;
        message->content_length = file_length;
        size_t log_length = sprintf((char*)hex_buffer, "%s /%s length %ld\n========\n",
                                    message->method,
                                    message->filename,
                                    message->content_length);
        pthread_mutex_lock(&sharedMemory->offset_mutex);
        int offset = sharedMemory->log_offset;
        sharedMemory->log_offset += log_length;
        pthread_mutex_unlock(&sharedMemory->offset_mutex);
        pwrite(sharedMemory->log_fd, hex_buffer,log_length, offset);
        return; 
    }else{
        fd = openRDONLY(message);
        stat(message->filename, &fileStat);
        file_length = fileStat.st_size;
        message->content_length = file_length;
    }
    size_t log_length = sprintf((char*)hex_buffer, "%s /%s length %ld",
                                message->method,
                                message->filename,
                                message->content_length);
    hex_offset += log_length;
    size_t lines = (int)file_length / 20;
    size_t rem = file_length % 20;
    log_length += lines * ( 8 + (20 * 3) + 1); 
    if (rem > 0) log_length += 8 + (rem * 3) + 1;
    log_length += 10;
    pthread_mutex_lock(&sharedMemory->offset_mutex);
    int offset = sharedMemory->log_offset;
    sharedMemory->log_offset += log_length;
    pthread_mutex_unlock(&sharedMemory->offset_mutex);
    if(file_length == 0){
        bytesWrote += pwrite(sharedMemory->log_fd, hex_buffer, hex_offset, offset);
    }
    while((bytesRead = read(fd, buffer, 400)) != 0){//Grab large chunk of file
        if(bytesRead < 0){  
            break;
        }   
        for(int i = 0; i < bytesRead; ++i){
            if(i % 20 == 0){
                hex_offset += sprintf((char*)hex_buffer + hex_offset, "\n%08d", bytesTotal);
                bytesTotal += 20;
            }
            hex_offset += snprintf((char*)hex_buffer + hex_offset, 100, " %02x", buffer[i]);
        }
        bytesWrote += pwrite(sharedMemory->log_fd, hex_buffer, hex_offset, offset+bytesWrote);
        memset(hex_buffer, 0, BUFFER_SIZE);
        hex_offset =0;
    }
    bytesWrote += pwrite(sharedMemory->log_fd, "\n========\n",10, offset+bytesWrote);
    close(fd);
}

void log_failure(struct httpObject* message, void* threadArgs){
    ThreadArg *sharedMemory = (ThreadArg*) threadArgs; 
    int size = sprintf(message->response, "FAIL: %s /%s %s --- response %d\n========\n",
             message->method,
             message->filename,
             message->httpversion,
             message->status_code);
    pthread_mutex_lock(&sharedMemory->offset_mutex);
    int offset = sharedMemory->log_offset;
    sharedMemory->log_offset += size;
    pthread_mutex_unlock(&sharedMemory->offset_mutex);
    pwrite(sharedMemory->log_fd, message->response, size, offset);
}