#include<err.h>
#include<arpa/inet.h>
#include<netdb.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>
#include<sys/select.h>
#include<unistd.h>
#include<getopt.h>
#include<ctype.h>
#include<pthread.h>
#include"list.h"
#include<stdbool.h>
#include<poll.h>
#include<signal.h>
#include <sys/time.h>

#define BUFFER_SIZE 4000
#define SECS_PER_HEALTHCHECK 4
#define INTERNAL_ERROR_RESPONSE "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"
typedef struct server_t{
    int fd;
    bool valid;
    size_t numRequests;
    size_t numErrs;
} Server;

typedef struct threadArg_t{
    pthread_t* threads;      //list of worker threads
    List* client_queue;
    Server* servers;
    size_t servers_length;
    pthread_mutex_t queue_mutex;
    pthread_cond_t wake_threads;
    pthread_cond_t healthcheck_cond;
    pthread_mutex_t health_check_lock;
    int lb_fd;
    int optimal_server_fd;
    int optimal_server_index; 
    int connection_counter;
    int connection_max;
} ThreadArg;


void* serveClient(void* threadArgs);    
void* healthCheck(void* threadArgs);

/*
 * client_connect takes a port number and establishes a connection as a client.
 * connectport: port number of server to connect to
 * returns: valid socket if successful, -1 otherwise
 */
int client_connect(uint16_t connectport) {
    int connfd;
    struct sockaddr_in servaddr;

    connfd=socket(AF_INET,SOCK_STREAM,0);
    if (connfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(connectport);

    /* For this assignment the IP address can be fixed */
    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    if(connect(connfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0)
        return -1;
    return connfd;
}

/*
 * server_listen takes a port number and creates a socket to listen on 
 * that port.
 * port: the port number to receive connections
 * returns: valid socket if successful, -1 otherwise
 */
int server_listen(int port) {
    int listenfd;
    int enable = 1;
    struct sockaddr_in servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
        return -1;
    if (bind(listenfd, (struct sockaddr*) &servaddr, sizeof servaddr) < 0)
        return -1;
    if (listen(listenfd, 500) < 0)
        return -1;
    return listenfd;
}

/*
 * bridge_connections send up to 100 bytes from fromfd to tofd
 * fromfd, tofd: valid sockets
 * returns: number of bytes sent, 0 if connection closed, -1 on error
 */
int bridge_connections(int fromfd, int tofd) {
    char recvline[4000];
    int n = recv(fromfd, recvline, 100, 0);
    if (n < 0) {
        //printf("connection error receiving\n");
        return -1;
    } else if (n == 0) {
        //printf("receiving connection ended\n");
        return 0;
    }
    recvline[n] = '\0';
    ////printf("%s", recvline);
    n = send(tofd, recvline, n, 0);
    if (n < 0) {
        //printf("connection error sending\n");
        return -1;
    } else if (n == 0) {
        //printf("sending connection ended\n");
        return 0;
    }
    return n;
}

/*
 * bridge_loop forwards all messages between both sockets until the connection
 * is interrupted. It also prints a message if both channels are idle.
 * sockfd1, sockfd2: valid sockets
 */
void bridge_loop(int sockfd1, int sockfd2) {
    fd_set set;
    struct timeval timeout;

    int fromfd, tofd;
    while(1) {
        // set for select usage must be initialized before each select call
        // set manages which file descriptors are being watched
        FD_ZERO (&set);
        FD_SET (sockfd1, &set);
        FD_SET (sockfd2, &set);

        // same for timeout
        // max time waiting, 5 seconds, 0 microseconds
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        // select return the number of file descriptors ready for reading in set
        switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
            case -1:
                //printf("error during select, exiting\n");
                return;
            case 0:
                //printf("both channels are idle, waiting again\n");
                return;
            default:
                if (FD_ISSET(sockfd1, &set)) {
                    fromfd = sockfd1;
                    tofd = sockfd2;
                } else if (FD_ISSET(sockfd2, &set)) {
                    fromfd = sockfd2;
                    tofd = sockfd1;
                } else {
                    //printf("this should be unreachable\n");
                    return;
                }
        }
        if (bridge_connections(fromfd, tofd) <= 0)
            return;
    }
}



int main(int argc, char** argv){
    ThreadArg *threadArgs = malloc(sizeof(ThreadArg));
    List queue = newList();
    threadArgs->client_queue = &queue;
    int num_threads = 4;
    int c;
    int er;

    pthread_mutex_init(&threadArgs->queue_mutex, NULL);
    pthread_mutex_init(&threadArgs->health_check_lock, NULL);
    pthread_cond_init(&threadArgs->wake_threads, NULL);
    pthread_cond_init(&threadArgs->healthcheck_cond, NULL);
    threadArgs->connection_max = 5;
    while ((c = getopt(argc, argv, "N:R:")) != -1){
        switch (c) {
            case 'N':
                er = sscanf(optarg,"%d",&num_threads);
                if(num_threads < 1){
                    return EXIT_FAILURE;
                }
                if(er != 1){
                    return EXIT_FAILURE;
                }
                break;
            case 'R':
                er = sscanf(optarg,"%d",&threadArgs->connection_max);
                if(er != 1){
                    return EXIT_FAILURE;
                }
                if(threadArgs->connection_max <= 0){
                    return EXIT_FAILURE;
                }
                break;
            default:
                ////printf("ERROR: invalid args");
                return EXIT_FAILURE;
         }
    }
    char* port = argv[optind++];
    threadArgs->servers_length = argc - optind;
    Server servers[threadArgs->servers_length];
    threadArgs->servers = servers;
    //printf("\nnumber of servers = %ld\n",threadArgs->servers_length);
    for(size_t i = 0; i < threadArgs->servers_length; i++){
        servers[i].fd = atoi(argv[optind++]);
        if(servers[i].fd < 0){
            return EXIT_FAILURE;
        }
        servers[i].valid = true;
        servers[i].numErrs = 0;
        servers[i].numRequests = 0;
        //printf("server at %ld is = %d\n", i, servers[i].fd);
    }

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
    //Doing a healthcheck
    //Finished healthcheck
    for (int idx = 0; idx < num_threads; ++idx) { 
        int err = pthread_create(&threads[idx], NULL, serveClient, (void*)threadArgs);
        if (err == -1){
            //printf("Thread error");
            return -1;
        }
    }
    pthread_t health_thread;
    pthread_create(&health_thread, NULL, healthCheck, (void*) threadArgs);
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

void calculate_optimal_server(ThreadArg* threadArgs){
    Server* servers = threadArgs->servers;
    //printf("\ncalculating optimal server\n");
        int smallest_index = -1;
        for(size_t i = 0; i < threadArgs->servers_length; ++i) {
            if (servers[i].valid){
                if(smallest_index == -1){
                    smallest_index = i;
                }
                if (servers[i].numRequests < servers[smallest_index].numRequests){ //get smallest
                    smallest_index = i;
                }else if (servers[i].numRequests == servers[smallest_index].numRequests){
                    if (servers[i].numErrs < servers[smallest_index].numErrs){
                        smallest_index = i;
                    }
                }
            }else{
                    //printf("Server %d is invalid",servers[i].fd);
            }
        }
        if(smallest_index < 0){
            threadArgs->optimal_server_fd = -1;
            threadArgs->optimal_server_index = -1;
        }else{
            threadArgs->optimal_server_fd = servers[smallest_index].fd;
            threadArgs->optimal_server_index = smallest_index;
            //printf("\nOptimal server is = %d | Valid = %d \n",threadArgs->optimal_server_fd, servers[smallest_index].valid);
        }
}


void* healthCheck(void* threadArg){
    ThreadArg* threadArgs = (ThreadArg*) threadArg;
    fd_set set;
    struct timeval timeout;
    while (true){
        //printf("--------------------------------------------------\nrunning healthcheck\n");
        char buff[200];
        Server* servers = threadArgs->servers;
        for(size_t i = 0; i < threadArgs->servers_length; ++i) {
            int requests, errs;
            int connfd;
            ssize_t bytesRead = 0;
            int buff_offset = 0;
            int connectport = servers[i].fd;
            //printf("connection port = %d\n", connectport);
            if ((connfd = client_connect(connectport)) < 0) {// connect to the server
                servers[i].valid = false;
                continue;
            }else{
                servers[i].valid = true;
            }
            dprintf(connfd,"GET /healthcheck HTTP/1.1\r\n\r\n");

            FD_ZERO(&set);
            FD_SET(connfd, &set);
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;
            switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
            case -1:
                //printf("case 1");
                servers[i].valid = false;
                continue;
            case 0:
                //printf("case 2");
                servers[i].valid = false;
                continue;
            default:
                //printf("case 3");
                break;
            }
            while( (bytesRead = recv(connfd, buff + buff_offset, BUFFER_SIZE-1, 0)) != 0){ 
                if(bytesRead < 0){
                    //printf("error on reading");
                    servers[i].valid = false;
                    continue;
                } 
                buff_offset += bytesRead;
            }

            char* header_end = strstr(buff, "\r\n\r\n");
            if(header_end == NULL){
                servers[i].valid = false;
                continue;
            }
            buff[buff_offset] = '\0';
            int status_code;
            sscanf(buff,"%*s %d",&status_code);
            if(status_code != 200){
                servers[i].valid = false;
                continue;
            }
            int nums_read = sscanf(header_end,"%d\n%d", &errs, &requests);
            if (nums_read != 2){
                servers[i].valid = false;
                continue;
            }
            //printf("Requests = %d, errs = %d\n\n", requests, errs);
            servers[i].numRequests = requests;
            servers[i].numErrs = errs;
            close(connfd);
        }
        calculate_optimal_server(threadArgs);
        //from stack overflow https://stackoverflow.com/questions/1486833/pthread-cond-timedwait
        struct timespec timeToWait;
        struct timeval now;
        gettimeofday(&now,NULL);
        timeToWait.tv_sec = now.tv_sec + 4;
        timeToWait.tv_nsec = (now.tv_usec + 1000UL * 4) * 1000UL;
        pthread_mutex_lock(&threadArgs->health_check_lock);
        pthread_cond_timedwait(&threadArgs->healthcheck_cond, &threadArgs->health_check_lock, &timeToWait);
        pthread_mutex_unlock(&threadArgs->health_check_lock);
    }
}


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
        int connfd;
        int connectport;
        sharedMemory->connection_counter++;
        if(sharedMemory->connection_counter >= sharedMemory->connection_max){
            pthread_cond_signal(&sharedMemory->healthcheck_cond);
        }
        connectport = sharedMemory->optimal_server_fd;   //server port
        //printf("\n PORT SENT TO IS : %d \n", connectport);
        if(connectport < 0){
            dprintf(client_sockd,INTERNAL_ERROR_RESPONSE);
            close(client_sockd);
            continue;
        }
        while( (connfd = client_connect(connectport)) < 0) {
            //printf("--------------------------hit connection error--------------------_");
            sharedMemory->servers[sharedMemory->optimal_server_index].valid = false;
            calculate_optimal_server(sharedMemory);
            connectport = sharedMemory->optimal_server_fd;
            if( sharedMemory->optimal_server_fd < 0){
                dprintf(client_sockd,INTERNAL_ERROR_RESPONSE);
                close(client_sockd);
            }
        }
        bridge_loop(client_sockd, connfd);
        close(client_sockd);
        close(connectport);
    }
}
