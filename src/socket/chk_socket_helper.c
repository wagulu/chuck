#include "socket/chk_socket_helper.h"
#include "util/chk_error.h"
#include "util/chk_log.h"

#ifndef  cast
# define  cast(T,P) ((T)(P))
#endif

int32_t easy_listen(int32_t fd,chk_sockaddr *server) {
	if(easy_bind(fd,server) != 0)
		 return chk_error_bind;
	if(listen(fd,SOMAXCONN) != 0){
        CHK_SYSLOG(LOG_ERROR,"listen() failed errno:%d",errno);        
		return chk_error_listen;
    }
	return chk_error_ok;
}

int32_t easy_connect(int32_t fd,chk_sockaddr *server,chk_sockaddr *local) {
	int32_t ret;
	if(local && chk_error_ok != easy_bind(fd,local))
	   return chk_error_bind;
    if(server->addr_type == SOCK_ADDR_IPV4)    
	   ret = connect(fd,cast(struct sockaddr*,server),sizeof(server->in));
	else if(server->addr_type == SOCK_ADDR_UN)
       ret = connect(fd,cast(struct sockaddr*,server),sizeof(server->un));
    else{
        CHK_SYSLOG(LOG_ERROR,"invaild address type");        
        return chk_error_invaild_addr_type;
    }
    
    if(ret == chk_error_ok || errno == EINPROGRESS)
        return chk_error_ok;
    else{
        CHK_SYSLOG(LOG_ERROR,"connect() failed errno:%d",errno);
        return chk_error_connect;
    }
}

int32_t easy_bind(int32_t fd,chk_sockaddr *addr) {
    int32_t ret = chk_error_ok;
    if(addr->addr_type == SOCK_ADDR_IPV4)
        ret = bind(fd,(struct sockaddr*)addr,sizeof(addr->in));
    else if(addr->addr_type == SOCK_ADDR_UN)
        ret = bind(fd,(struct sockaddr*)addr,sizeof(addr->un));
    else{
        CHK_SYSLOG(LOG_ERROR,"invaild address type"); 
        return chk_error_invaild_addr_type;
    }

    if(ret != chk_error_ok) {
        CHK_SYSLOG(LOG_ERROR,"bind() failed errno:%d",errno);
        return chk_error_bind;
    }

    return chk_error_ok;    
}

int32_t easy_addr_reuse(int32_t fd,int32_t yes) {
	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))){
        CHK_SYSLOG(LOG_ERROR,"setsockopt(SOL_SOCKET,SO_REUSEADDR) failed errno:%d",errno); 
		return chk_error_setsockopt;
    }
	return chk_error_ok;	
}

int32_t easy_noblock(int32_t fd,int32_t noblock) {
    int32_t flags;
	if((flags = fcntl(fd, F_GETFL, 0)) == -1){
        CHK_SYSLOG(LOG_ERROR,"fcntl(F_GETFL) failed errno:%d",errno);        
    	return chk_error_fcntl;
	}
    if(!noblock){
        flags &= (~O_NONBLOCK);
    }else {
        flags |= O_NONBLOCK;
    }

    if(0 != fcntl(fd, F_SETFL, flags)) {
        CHK_SYSLOG(LOG_ERROR,"fcntl(F_SETFL) failed errno:%d",errno);        
        return chk_error_fcntl;
    }
    return chk_error_ok;	
}

int32_t easy_close_on_exec(int32_t fd) {
	int32_t flags;;
    if((flags = fcntl(fd, F_GETFD, 0)) == -1){
        CHK_SYSLOG(LOG_ERROR,"fcntl(F_GETFD) failed errno:%d",errno);         
    	return chk_error_fcntl;
	}
	
    if(0 != fcntl(fd, F_SETFD, flags|FD_CLOEXEC)) {
        CHK_SYSLOG(LOG_ERROR,"fcntl(F_SETFD) failed errno:%d",errno);        
        return chk_error_fcntl;
    }
    return chk_error_ok;        
}

int32_t easy_sockaddr_ip4(chk_sockaddr *addr,const char *ip,uint16_t port) {
    memset(cast(void*,addr),0,sizeof(*addr));
    addr->addr_type = SOCK_ADDR_IPV4;
    addr->in.sin_family = AF_INET;
    addr->in.sin_port = htons(port);
    if(inet_pton(AF_INET,ip,&addr->in.sin_addr) < 0){ 
        CHK_SYSLOG(LOG_ERROR,"inet_pton(AF_INET) failed errno:%d",errno);          
        return chk_error_invaild_sockaddr;
    }
    return chk_error_ok;
}

int32_t easy_sockaddr_un(chk_sockaddr *addr,const char *path) {
    memset(cast(void*,addr),0,sizeof(*addr));
    addr->addr_type = SOCK_ADDR_UN;    
    addr->un.sun_family = AF_LOCAL;
    strncpy(addr->un.sun_path,path,sizeof(addr->un.sun_path)-1);
    return chk_error_ok;
}

//get the first ipv4 address of name
int32_t easy_hostbyname_ipv4(const char *name,char *host,size_t len) {
   
#ifdef _MACH
    struct hostent *result;
    if(NULL == (result = gethostbyname(name))){
        CHK_SYSLOG(LOG_ERROR,"gethostbyname_r() failed errno:%d",h_errno);       
        return chk_error_invaild_hostname;
    }
    if(inet_ntop(AF_INET, result->h_addr_list[0],host, len) != NULL){
        CHK_SYSLOG(LOG_ERROR,"inet_ntop() failed errno:%d",errno);         
        return chk_error_ok;
    }
    return chk_error_invaild_hostname;
#else    
    int     h_err;
    char    buf[8192];
    struct hostent ret, *result;
    if(gethostbyname_r(name, &ret, buf, 8192, &result, &h_err) != 0){
        CHK_SYSLOG(LOG_ERROR,"gethostbyname_r() failed errno:%d",h_err);        
        return chk_error_invaild_hostname;
    }
    if(inet_ntop(AF_INET, result->h_addr_list[0],host, len) != NULL){
        CHK_SYSLOG(LOG_ERROR,"inet_ntop() failed errno:%d",errno);        
        return chk_error_ok;
    }
    return chk_error_invaild_hostname;
#endif
}