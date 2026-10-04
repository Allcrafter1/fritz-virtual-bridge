// SPDX-License-Identifier: MIT OR Apache-2.0
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(int argc,char **argv){
    if(argc<2||argc>3){fprintf(stderr,"usage: %s COMMAND [SOCKET]\n",argv[0]);return 2;}
    const char *path=argc==3?argv[2]:"/var/tmp/aha-virtual-provider.ctl";
    if(strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return 2;
    int socket_fd=socket(AF_UNIX,SOCK_SEQPACKET,0);if(socket_fd<0)return 3;
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    struct timeval timeout={3,0};
    setsockopt(socket_fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    /* A full/stale local accept queue can otherwise leave connect() blocked
     * forever.  The watchdog must always regain control so it can restart the
     * isolated AHA process. */
    setsockopt(socket_fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if(connect(socket_fd,(struct sockaddr*)&address,sizeof(address))){close(socket_fd);return 4;}
    char command[80];int command_length=snprintf(command,sizeof(command),"%s\n",argv[1]);
    if(command_length<=0||command_length>=(int)sizeof(command)||
       send(socket_fd,command,(size_t)command_length,MSG_NOSIGNAL)!=command_length)return 5;
    char reply[1024];ssize_t length;
    while((length=recv(socket_fd,reply,sizeof(reply),0))>0){
        if(write(STDOUT_FILENO,reply,(size_t)length)!=length)return 6;
        if(strcmp(argv[1],"WATCH"))break;
    }
    close(socket_fd);return length<0?7:0;
}
