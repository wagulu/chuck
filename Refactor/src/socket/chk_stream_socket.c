#define _CORE_
#include <assert.h>
#include "socket/chk_stream_socket.h"
#include "util/chk_bytechunk.h"
#include "socket/chk_socket_helper.h"
#include "util/chk_error.h"
#include "event/chk_event_loop.h"

#define MAX_WBAF          512
#define MAX_SEND_SIZE     1024*16

#ifndef  cast
# define  cast(T,P) ((T)(P))
#endif

//status
enum{
	SOCKET_CLOSE     = 1 << 1,  //完全关闭
	SOCKET_HCLOSE    = 1 << 2,  //读关闭,写等剩余包发完关闭
	SOCKET_INLOOP    = 1 << 3,
};

struct chk_stream_socket {
	_chk_handle;
	chk_stream_socket_option option;
	struct iovec         wsendbuf[MAX_WBAF];
    struct iovec         wrecvbuf[2];
    uint32_t             status;
    uint32_t             next_recv_pos;
    chk_bytechunk       *next_recv_buf;
    void                *ud;        
    chk_list             send_list;             //待发送的包
    chk_timer           *timer;                 //用于最后的发送处理
    chk_stream_socket_cb cb;
};

/*
* 默认解包器,将已经接收到的数据全部置入chk_bytebuffer
*/
typedef struct default_decoder {
	void (*update)(chk_decoder*,chk_bytechunk *b,uint32_t spos,uint32_t size);
	chk_bytebuffer *(*unpack)(chk_decoder*,int32_t *err);
	void (*dctor)(chk_decoder*);
	uint32_t       spos;
	uint32_t       size;
	chk_bytechunk *b;
	void          *ud;
}default_decoder;

static void default_update(chk_decoder *d,chk_bytechunk *b,uint32_t spos,uint32_t size) {
	cast(default_decoder*,d)->spos = spos;
	cast(default_decoder*,d)->b    = b;
	cast(default_decoder*,d)->size = size;
}

static chk_bytebuffer *default_unpack(chk_decoder *d,int32_t *err) {
	default_decoder *_d  = cast(default_decoder*,d);
	chk_bytebuffer  *ret = NULL;
	*err = 0;
	if(_d->b) {
		ret = chk_bytebuffer_new(_d->b,_d->spos,_d->size);
		_d->b = NULL;
	}
	return ret;
}

static default_decoder *default_decoder_new() {
	default_decoder *d = calloc(1,sizeof(*d));
	d->update = default_update;
	d->unpack = default_unpack;
	return d;
}

//发送数据完成,更新缓冲信息
static inline void update_send_list(chk_stream_socket *s,int32_t _bytes) {
	chk_bytebuffer *b;
	chk_bytechunk  *head;
	uint32_t        bytes = cast(uint32_t,_bytes);
	uint32_t        size;
	for(;bytes;) {
		b = cast(chk_bytebuffer*,chk_list_begin(&s->send_list));
		assert(b);
		if(bytes >= b->datasize) {
			//一个buffer已经发送完毕,可以删除
			chk_list_pop(&s->send_list);
			bytes -= b->datasize;
			chk_bytebuffer_del(b);
		}else {
			//只完成一个buffer中部分数据的发送
			do {
				head = b->head;
				size = head->cap - b->spos;
				size = size > bytes ? bytes:size;
				bytes -= size;
				b->spos += size;
				b->datasize -= size;
				if(b->spos >= head->cap) {
					//发送完一个chunk
					b->spos = 0;
					b->head = chk_bytechunk_retain(head->next);
					chk_bytechunk_release(head);
				}
			}while(bytes);
		}
	}
}

//准备缓冲用于发起写请求
static inline int32_t prepare_send(chk_stream_socket *s) {
	int32_t          i = 0;
	chk_bytebuffer  *b = cast(chk_bytebuffer*,chk_list_begin(&s->send_list));
	chk_bytechunk   *chunk;
	uint32_t    datasize,size,pos,send_size;
	send_size = 0;
	while(b && i < MAX_WBAF && send_size < MAX_SEND_SIZE) {
		pos   = b->spos;
		chunk = b->head;
		datasize = b->datasize;
		while(i < MAX_WBAF && chunk && datasize) {
			s->wsendbuf[i].iov_base = chunk->data + pos;
			size = chunk->cap - pos;
			size = size > datasize ? datasize:size;
			datasize    -= size;
			send_size   += size;
			s->wsendbuf[i].iov_len = size;
			++i;
			chunk = chunk->next;
			pos = 0;
		}
		if(send_size < MAX_SEND_SIZE) 
			b = cast(chk_bytebuffer*,cast(chk_list_entry*,b)->next);
	}
	return i;
}

//准备缓冲用于发起读请求
static inline int32_t prepare_recv(chk_stream_socket *s) {
	chk_bytechunk  *chunk;
	int32_t         i = 0;
	uint32_t        recv_size,pos;
	if(!s->next_recv_buf) {
		s->next_recv_buf = chk_bytechunk_new(NULL,s->option.recv_buffer_size);
		s->next_recv_pos = 0;
	}
	for(pos = s->next_recv_pos,chunk = s->next_recv_buf;;) {
		recv_size = chunk->cap - pos;
		s->wrecvbuf[i].iov_len  = recv_size;
		s->wrecvbuf[i].iov_base = chunk->data + pos;
		++i;
		if(recv_size != s->option.recv_buffer_size) {
			pos = 0;
			if(!chunk->next) 
				chunk->next = chk_bytechunk_new(NULL,s->option.recv_buffer_size);
			chunk = chunk->next;
		}else break;
	}
	return i;
}

//数据接收完成,更新缓冲信息
static inline void update_next_recv_pos(chk_stream_socket *s,int32_t bytes) {
	uint32_t       size;
	chk_bytechunk *head;
	do {
		head = s->next_recv_buf;
		size = head->cap - s->next_recv_pos;
		size = size > bytes ? bytes:size;
		s->next_recv_pos += size;
		bytes -= size;
		if(s->next_recv_pos >= head->cap) {
			s->next_recv_pos = 0;
			head = s->next_recv_buf;			
			if(!head->next)
				head->next = chk_bytechunk_new(NULL,s->option.recv_buffer_size);
			s->next_recv_buf = chk_bytechunk_retain(head->next);
			chk_bytechunk_release(head);					
		}
	}while(bytes);
}

static void process_read(chk_stream_socket *s) {
	int32_t bc,bytes,unpackerr;
	chk_decoder *decoder;
	chk_bytebuffer *b;
	bc    = prepare_recv(s);
	errno = 0;
	bytes = TEMP_FAILURE_RETRY(readv(s->fd,&s->wrecvbuf[0],bc));
	if(bytes > 0 ) {
		decoder = s->option.decoder;
		decoder->update(decoder,s->next_recv_buf,s->next_recv_pos,bytes);
		do {
			unpackerr = 0;
			if((b = decoder->unpack(decoder,&unpackerr))) {
				s->cb(s,0,b);
				chk_bytebuffer_del(b);
				if(s->status & SOCKET_CLOSE || s->status & SOCKET_HCLOSE) 
					break;
			}else {
				if(unpackerr) s->cb(s,unpackerr,NULL);
				break;
			}
		}while(1);
		if(!(s->status & SOCKET_CLOSE || s->status & SOCKET_HCLOSE))
			update_next_recv_pos(s,bytes);
	}else s->cb(s,errno,NULL);
}


static void release_socket(chk_stream_socket *s) {
	chk_bytebuffer  *b;
	chk_events_remove(cast(chk_handle*,s));	
	if(s->next_recv_buf) chk_bytechunk_release(s->next_recv_buf);
	if(s->option.decoder) chk_decoder_del(s->option.decoder);
	if(s->timer) chk_timer_unregister(s->timer);
	while((b = cast(chk_bytebuffer*,chk_list_pop(&s->send_list))))
		chk_bytebuffer_del(b);
	close(s->fd);
	free(s);
}


static void process_write(chk_stream_socket *s) {
	int32_t bc,bytes;
	bc = prepare_send(s);
	if((bytes = TEMP_FAILURE_RETRY(writev(s->fd,&s->wsendbuf[0],bc))) > 0)
		update_send_list(s,bytes);
		//没有数据需要发送了,停止写监听
		if(chk_list_empty(&s->send_list)) {
		 	if(s->status & SOCKET_HCLOSE)
				s->status |= SOCKET_CLOSE;
			else
		 		chk_disable_write(cast(chk_handle*,s));
		}	
	else {
		assert(errno != EAGAIN);
		if(s->status & SOCKET_HCLOSE)
			s->status |= SOCKET_CLOSE;
		else
			chk_disable_write(cast(chk_handle*,s));
	}
}

static void on_events(chk_handle *h,int32_t events) {
	chk_stream_socket *s = cast(chk_stream_socket*,h);
	if(!s->loop || s->status & SOCKET_CLOSE)
		return;
	if(events == CHK_EVENT_ECLOSE) {
		//通知loop关闭
		s->cb(s,CHK_ELOOPCLOSE,NULL);
		return;
	}
	do {
		s->status |= SOCKET_INLOOP;
		if(events & CHK_EVENT_READ){
			process_read(s);	
			if(s->status & SOCKET_CLOSE) 
				break;								
		}		
		if(s->loop && (events & CHK_EVENT_WRITE))
			process_write(s);			
		s->status ^= SOCKET_INLOOP;
	}while(0);
	if(s->status & SOCKET_CLOSE) {
		release_socket(s);		
	}
}

static int32_t loop_add(chk_event_loop *e,chk_handle *h,chk_event_callback cb) {
	int32_t ret;
	chk_stream_socket *s = cast(chk_stream_socket*,h);
	if(!e || !h || !cb || s->status & SOCKET_CLOSE || s->status & SOCKET_HCLOSE)
		return -1;
	if(!chk_list_empty(&s->send_list))
		ret = chk_events_add(e,h,CHK_EVENT_READ) || chk_events_add(e,h,CHK_EVENT_WRITE);
	else
		ret = chk_events_add(e,h,CHK_EVENT_READ);
	if(ret == 0) {
		easy_noblock(h->fd,1);
		s->cb = cast(chk_stream_socket_cb,cb);
	}
	return ret;
}

chk_stream_socket *chk_stream_socket_new(int32_t fd,chk_stream_socket_option *op) {
	chk_stream_socket *s = calloc(1,sizeof(*s));
	easy_close_on_exec(fd);
	s->fd = fd;
	s->on_events = on_events;
	s->handle_add = loop_add;
	s->option = *op;
	if(!s->option.decoder) 
		s->option.decoder = cast(chk_decoder*,default_decoder_new());
	return s;
}

int32_t last_timer_cb(uint64_t tick,void*ud) {
	//发送已经超时,直接释放
	chk_stream_socket *s = cast(chk_stream_socket*,ud);
	s->timer   = NULL;
	//timer事件在所有套接口事件之后才处理,所以这里释放是安全的
	release_socket(s);
	return -1;
}

void chk_stream_socket_close(chk_stream_socket *s) {
	if((s->status & SOCKET_CLOSE) || (s->status & SOCKET_HCLOSE)) 
		return;
	
	if(!chk_list_empty(&s->send_list) && s->loop) {
		s->status |= SOCKET_HCLOSE;
		chk_disable_read(cast(chk_handle*,s));
		shutdown(s->fd,SHUT_RD);
		//数据还没发送完,设置5秒超时等待数据发送
		s->timer = chk_loop_addtimer(s->loop,5000,last_timer_cb,s);
	}else {
		s->status |= SOCKET_CLOSE;		
		if(!(s->status & SOCKET_INLOOP)){
			release_socket(s);
		}
	}
}

int32_t chk_stream_socket_send(chk_stream_socket *s,chk_bytebuffer *b) {
	int32_t ret = 0;
	do {
		if((s->status & SOCKET_CLOSE) || (s->status & SOCKET_HCLOSE)) {
			chk_bytebuffer_del(b);	
			ret = -1;
			break;
		}
		chk_list_pushback(&s->send_list,cast(chk_list_entry*,b));
		if(!chk_is_write_enable(cast(chk_handle*,s))) 
			chk_enable_write(cast(chk_handle*,s));
	}while(0);
	return ret;	
}

void chk_stream_socket_setUd(chk_stream_socket *s,void*ud) {
	s->ud = ud;
}

void *chk_stream_socket_getUd(chk_stream_socket *s) {
	return s->ud;
}