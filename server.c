//compile with:
	//gcc -Wall -fopenmp server.c -o s

#define _GNU_SOURCE 	//for splice
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <omp.h>
#include <getopt.h>
#include <fcntl.h>
#include <limits.h>


#define MAXEVENTS 64

typedef struct{
	int fd;
	int pipefd[2];	//two ends of the pipe
} epoll_data;

int make_bound(const char* port);
int epoll_data_init(epoll_data *epd, int fd); 
int make_non_blocking(int sfd);
void epoll_data_close(epoll_data *epd); 
int echo(epoll_data *epd); 
void echo_harder(epoll_data *epd);

int main(int argc, char** argv) {
	int efd;	//file descriptor for epoll 
	int sfd, s;	//listening socket and listen return
	char *port = "7000";
	struct epoll_event event;	//holds events and data (in epoll_data_t data)
	struct epoll_event *events;
	epoll_data *data;
		
	//make and bind the socket
	sfd = make_bound(port);
	if(sfd == -1) {
		return 2;	
	}

	//start listening
	s = listen(sfd, SOMAXCONN);
	if(s == -1) {
		perror("listen");
		return 3;
	}

	//register the epoll structure
	efd  = epoll_create1(0);	//standard epoll, no flags
	if(efd == -1) {
		perror("epoll_create1");
		return 4;	
	}

	data = calloc(1, sizeof(epoll_data));	//initialize all bits to 0
	epoll_data_init(data, sfd);		//initialize fd to work with non-blocking
	event.data.ptr = data;			//point event.data to our epoll_data struct
	//setup event for 
		//EPOLLIN: read operations
		//EPOLLET: set to use Edge Triggered behavior (default Level Triggered) on the fd. 
			//used for non-blocking read/write to work with EAGAIN
		//EPOLLEXCLUSIVE: sets an exllusive wakeup mode for the efd, so all the efds don't 
			//receive the event	
	event.events = EPOLLIN | EPOLLET | EPOLLEXCLUSIVE;
	s = epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &event);		//associate efd with sfd
	if(s == -1) {
		perror("epoll_ctl");
		return 5;
	}

	//Buffer where events are retunred *no more than 64 at the same time)
	events = calloc(MAXEVENTS, sizeof(event));
	
//run the rest of the code on as many cores as the machine has, epoll_wait is thread safe
#pragma omp parallel
	while(1) {
		int n, i;

		//wait for an fd event, interrupt, or timeout from last parameter 
			//a number of MAXEVENTS can be returned at once 
			//-1 sets the timer to wait indefinfinitely, that means blocking
			//can also set to block for a specific amount of time (handy)
		n = epoll_wait(efd, events, MAXEVENTS, -1);	
		
		//loop through all the newly found fd events
		for(i = 0; i < n; i++) {
			//if there is an error or a hang up
			if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
				//a socket got closed
				epoll_data_close((epoll_data *) events[i].data.ptr);
				free(events[i].data.ptr);
				continue;
			} else if((events[i].events & EPOLLIN)) {	//there is something to read
				if(sfd == ((epoll_data *)events[i].data.ptr)->fd) {
					//We havea  notification on the listening socket, which means
						//one or more incoming connections
					while(1) {
						struct sockaddr in_addr;
						socklen_t in_len;
						int infd;
						char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

						in_len = sizeof in_addr;	//not sizeof(in_addr)?
						infd = accept(sfd, &in_addr, &in_len);
						if(infd == -1) {
							if((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
								//we finished proccessing all incoming connections
								break;
							} else {
								perror("accept");
								break;
							}
						}

						//gethostbyaddr/getservbyport but for ipv4/6
							//&in_addr: holds all info (ip/port)
							//in_len: size of the sockaddr
							//hbuf: will hold the host name
							//sbuf: will hold the service name
							//NI_NUMERICHOST: numeric form of the hostname is returned
							//NI_NUMERICSERV: numeric form of the service is returned
						s = getnameinfo(&in_addr, in_len, hbuf, sizeof hbuf, sbuf, sizeof sbuf, NI_NUMERICHOST | NI_NUMERICSERV);
						if(s == 0) {
							printf("Accepted connection on descriptor %d "
								"(host=%s, port=%s)\n", infd, hbuf, sbuf);	//wtf?
						}

						//make the incoming socket non-blocking and add it to the list
						//of fds to monitor
						s = make_non_blocking(infd);
						if(s == -1) {
							abort();
						}

						data = calloc(1, sizeof(epoll_data));
						epoll_data_init(data, infd);
					
						event.data.ptr = data;
						event.events = EPOLLIN | EPOLLOUT | EPOLLET;
						s = epoll_ctl(efd, EPOLL_CTL_ADD, infd, &event);
						if(s == -1) {
							perror("epoll_ctl");
							abort();
						}
					}
				
					continue;
				} else {
					//regular incoming message, echo it back
					echo((epoll_data *)event.data.ptr);
				}
			} else {
				//EPOLLOUT
				//this shouldn't happen because it means we're blocked
				//we are now notified that we can send the rest of the data
				echo_harder((epoll_data *)event.data.ptr);
			}
		}
	}
	
	free(events);
	close(sfd);
	return 0;	
}

//binds to appropriate address returned by dns search
int make_bound(const char *port) {
	struct addrinfo hints;	//expanded sockaddr object
	struct addrinfo *results, *rp;
	int s, sfd;

	memset(&hints, 0, sizeof(struct addrinfo));	//0 out the object's memory
	hints.ai_family = AF_UNSPEC;			//Return IPv4 and IPv6 choices
	hints.ai_socktype = SOCK_STREAM;		//TCP socket
	hints.ai_flags = AI_PASSIVE;			//All interfaces

	//gets the list of address names into a linked list, result points to the head
		//security risks involved with glibc implementation
	s = getaddrinfo(0, port, &hints, &results);
	if(s != 0) {
		perror("getaddrinfo");
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s)); //
	}
	
	for(rp = results; rp != 0; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
		if(sfd == -1) {
			continue;
		}

		int enable = 1;

		//allow reuse of local addresses (unless in use)
		if(setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1) {
			perror("Socket options failed");
			exit(EXIT_FAILURE);
		}

		s = bind(sfd, rp->ai_addr, rp->ai_addrlen);
		if(s == 0) {
			//binded successfully
			break;
		}

		close(sfd);	//needs #include <unistd.h>
	}
	
	if(!rp) {
		fprintf(stderr, "Unable to bind\n");
		return -1;
	}

	freeaddrinfo(results);
	return sfd;
}

int epoll_data_init(epoll_data *epd, int fd) {
	epd->fd = fd;
	if(pipe(epd->pipefd)) {
		perror("pipe");
		return -1;	
	}

	return 0;
}

int make_non_blocking(int sfd) {
	int flags, s;

	//sets flags to file access mode and the file status
	flags = fcntl(sfd, F_GETFL, 0);
	if(flags == -1) {
		perror("fcntl get");
		return -1;
	}

	//or equal operator, nooby dooope
	flags |= O_NONBLOCK;
	s = fcntl(sfd, F_SETFL, flags);
	if(s == -1) {
		perror("fcntl set");
		return -1;
	}

	return 0;
}

void epoll_data_close(epoll_data *epd) {
	close(epd->pipefd[0]);
	close(epd->pipefd[1]);
	close(epd->fd);
}

int echo(epoll_data *epd) {
	while(1) {
		//read max standard pipe allocation size
		int nr = splice(epd->fd, 0, epd->pipefd[1], 0, USHRT_MAX, SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK);
		if(nr == -1 && errno != EAGAIN) {
			perror("splice");
		}
		if(nr <= 0) {
			break;
		}

		printf("read: %d\n", nr);

		do{
			int ret = splice(epd->pipefd[0], 0, epd->fd, 0, nr, SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK);
			if(ret <= 0) {
				if(ret == -1 && errno != EAGAIN) {
					perror("splice2");
				}
				break;
			}

			printf("wrote: %d\n", ret);
			nr -= ret;
		} while(nr);
	}

	return 0;
}

void echo_harder(epoll_data *epd) {
	while(1) {
		int ret = splice(epd->pipefd[0], 0, epd->fd, 0, USHRT_MAX, SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK);
		if(ret <= 0) {
			if(ret == -1 && errno !=EAGAIN) {
				perror("splice");
			}
			break;
		}

		printf("wrote: %d\n", ret);
	}
}











