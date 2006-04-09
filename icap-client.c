/*
 *  Copyright (C) 2004 Christos Tsantilas
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include "request.h"
#include "net_io.h"
#include "cfg_param.h"
#include "debug.h"
 
//char *servername;
//char *modulename;

int TIMEOUT=300;




int create_request(request_t *req,char *servername,char *service,int reqtype){
    char buf[256];    

    if(reqtype!=ICAP_OPTIONS && reqtype!=ICAP_REQMOD && reqtype!=ICAP_RESPMOD)
	return CI_ERROR;

    req->type=reqtype;
    req->is_client_request=1;
    snprintf(buf,255,"%s icap://%s/%s ICAP/1.0",
	     ci_method_string(reqtype),servername,service);
    buf[255]='\0';
    add_header(req->head,buf);
    snprintf(buf,255,"Host: %s",servername);
    buf[255]='\0';
    add_header(req->head,buf);
    add_header(req->head,"User-Agent: C-ICAP-Client-Library/0.1");
    return CI_OK;
}

int ci_get_request_options(request_t *req,ci_header_list_t *h){
    char *pstr;

    if((pstr=get_header_value(h, "Preview"))!=NULL){
	req->preview=strtol(pstr,NULL,10);
	if(req->preview<0)
	    req->preview=0;
    }
    else
	req->preview=0;

    
    req->allow204=0;
    if((pstr=get_header_value(h, "Allow"))!=NULL){
	if(strtol(pstr,NULL,10) == 204)
	    req->allow204=1;
    }

    req->keepalive=1;
    if((pstr=get_header_value(h, "Connection"))!=NULL && strncmp(pstr,"close",5)==0){
	req->keepalive=0;
    }

    /*Moreover we are interested for the followings*/
    if((pstr=get_header_value(h, "Transfer-Preview"))!=NULL){
	/*Not implemented yet*/
    }

    if((pstr=get_header_value(h, "Transfer-Ignore"))!=NULL){
	/*Not implemented yet*/
    }
	
    if((pstr=get_header_value(h, "Transfer-Complete"))!=NULL){
	/*Not implemented yet*/
    }

    /*
      The headers Max-Connections and  Options-TTL are not needed in this client 
      but if this functions moves to a general client api must be implemented
    */
    
    return CI_OK;
}


int ci_writen(int fd,char *buf,int len,int timeout){
    int ret=0,remains;
    remains=len;
    while(remains){
	if((ret=ci_write(fd,buf,remains,timeout))<0)
	    return ret;
	buf+=ret;
	remains-=ret;
    }
    return len;
}



int ci_send_request_headers(request_t *req,int has_eof){
    int ret;
    ci_encaps_entity_t **elist,*e;
    ci_header_list_t *headers;
//    ci_headers_pack(req->head);
    
    ci_request_pack(req);
    ret=ci_writen(req->connection->fd,req->head->buf,req->head->bufused,TIMEOUT);

    elist=req->entities;
    while((e=*elist++)!=NULL){
	if(e->type==ICAP_REQ_HDR || e->type==ICAP_RES_HDR){
	    headers=( ci_header_list_t *)e->entity;
	    ret=ci_writen(req->connection->fd,headers->buf,headers->bufused,TIMEOUT);
	}
    }

    if(req->preview>0 && req->preview_data.used >0 ){
	ret=sprintf(req->wbuf,"%x\r\n",req->preview);
	ret=ci_writen(req->connection->fd,req->wbuf,ret,TIMEOUT);
	ret=ci_writen(req->connection->fd,req->preview_data.buf,req->preview,TIMEOUT);
	if(has_eof)
	    ret=ci_writen(req->connection->fd,"\r\n0; ieof\r\n\r\n",13,TIMEOUT);
	else
	    ret=ci_writen(req->connection->fd,"\r\n0\r\n\r\n",7,TIMEOUT);
    }

    return ret;
}


int process_encapsulated(request_t *req,char *buf){
     char *start;
     char *pos;
     char *end;
     int type=0,num=0,val=0;
     int hasbody=1;/*Assume that we have a resbody or reqbody or optbody*/
     start=buf+14;
     pos=start;
     while(*pos!='\0'){
	  while(!isalpha(*pos) && *pos!='\0') pos++;
	  type=get_encaps_type(pos,&val,&end);
	  if(num>5) /*In practice there is an error here .... */
	       break;
	  if(type==ICAP_NULL_BODY)
	       hasbody=0; /*We have not a body*/
	  req->entities[num++]=ci_request_alloc_entity(req,type,val);
	  pos=end;/* points after the value of entity....*/
     }
     req->hasbody=hasbody;
     return 0;
}


int read_encaps_header(request_t *req,ci_header_list_t *h,int size){
     int bytes=0,remains,readed=0;
     char *buf_end=NULL;
     printf("Going to read header with size:%d\n",size);
     if(!set_size_header(h,size))
	  return EC_500;
     buf_end=h->buf;
     
     if(req->pstrblock_read_len>0){
	  readed=(size>req->pstrblock_read_len?req->pstrblock_read_len:size);
	  memcpy(h->buf,req->pstrblock_read,readed);
	  buf_end=h->buf+readed;
	  if(size<=req->pstrblock_read_len){/*We have readed all this header.......*/
	       req->pstrblock_read=(req->pstrblock_read)+readed;
	       req->pstrblock_read_len=(req->pstrblock_read_len)-readed;
	  }
	  else{
	       req->pstrblock_read=NULL;
	       req->pstrblock_read_len=0;
	  }
     }

     remains=size-readed;
     while(remains>0){
	  if((bytes=ci_read(req->connection->fd,buf_end,remains,TIMEOUT))<0)
	       return bytes;
	  remains-=bytes;
	  buf_end+=bytes;
     }

     h->bufused=buf_end - h->buf; // -1 ;
     if(strncmp(buf_end-4,"\r\n\r\n",4)==0){
	  h->bufused-=2; /*eat the last 2 bytes of "\r\n\r\n"*/
     }
     return EC_100;
}

int read_encaps_headers(request_t *req){  
     int size,i,request_status=0;
     ci_encaps_entity_t *e=NULL;
     for(i=0;(e=req->entities[i])!=NULL;i++){
	  if(e->type>ICAP_RES_HDR) //res_body,req_body or opt_body so the end of the headers.....
	       return EC_100;

	  if(req->entities[i+1]==NULL)
	       return EC_400;

	  size=req->entities[i+1]->start-e->start;

	  if((request_status=read_encaps_header(req,(ci_header_list_t *)e->entity,size))!=EC_100)
	       return request_status;

	  if((request_status=ci_headers_unpack((ci_header_list_t *)e->entity))!=EC_100)
	       return request_status;
     }
     return EC_100;
}


enum chunk_status{READ_CHUNK_DEF, READ_CHUNK_DATA};

#define CI_NEEDS_MORE 2
/*
  maybe the wdata must moved to the request_t and write_to_module_pending must replace wdata_len
*/
int parse_chunk_data(request_t *req, char **wdata){
     char *end;
     int num_len,remains,tmp;
     int read_status=0;
     
     *wdata=NULL;
     
     if(req->write_to_module_pending){
	  /*We must not here if the chunk buffer did not flashed*/
	  return CI_ERROR;
     }
     req->write_to_module_pending=0; /*Does not needed but who knows ......*/
     
     while(1){
	  if(req->current_chunk_len==req->chunk_bytes_read)
	       read_status=READ_CHUNK_DEF;
	  else
	       read_status=READ_CHUNK_DATA;
	  
	  if(read_status==READ_CHUNK_DEF){	       
	       errno=0;
	       tmp=strtol(req->pstrblock_read,&end,16);
	       /*here must check for erron*/
	       if(tmp==0 && req->pstrblock_read==end ){ /*Oh .... an error ...*/
		    ci_debug_printf(5,"Parse error:count=%d,start=%c\n",
				    tmp,
				    req->pstrblock_read[0]);
		    return CI_ERROR; 
	       }	
	       num_len=end-req->pstrblock_read;	
	       if(req->pstrblock_read_len-num_len < 2){ 
		    return CI_NEEDS_MORE;
	       }

	       /*At this point the req->pstrblock_read must point to the start of a chunk eg to a "123\r\n"*/
	       req->chunk_bytes_read=0;
	       req->current_chunk_len=tmp;

	       if(req->current_chunk_len==0){
		    
		    if(*end==';'){ 
			 if(req->pstrblock_read_len-num_len < 11){
			      return CI_NEEDS_MORE;
			 }
			 
			 if(strncmp(end,"; ieof",6)!=0)
			      return CI_ERROR;
			 
			 req->eof_received=1;
			 return CI_EOF;
		    }
		    else{
			 if(req->pstrblock_read_len-num_len < 4){
			      return CI_NEEDS_MORE;
			 }
			 if(strncmp(end,"\r\n\r\n",4)!=0)
			      return CI_ERROR;
			 return CI_EOF;
		    }
	       }
	       else{
		    if(*end!='\r' || *(end+1)!='\n'){
			 return CI_ERROR;
		    }
		    read_status=READ_CHUNK_DATA;
		    req->pstrblock_read=end+2;
		    req->pstrblock_read_len-=(num_len+2);
		    /*include the \r\n end of chunk data*/
		    req->current_chunk_len+=2;
	       }
	  }
	  /*if we have data for service leaving this function now*/
	  if(req->write_to_module_pending)
	       return CI_OK;
	  
	  if(read_status==READ_CHUNK_DATA){
	       if(req->pstrblock_read_len<=0){
		    return CI_NEEDS_MORE;
	       }
	       
	       *wdata=req->pstrblock_read;
	       
	       remains=req->current_chunk_len-req->chunk_bytes_read;
	       if(remains<=req->pstrblock_read_len){/*we have all the chunks data*/
		    if(remains>2)
			 req->write_to_module_pending=remains-2;
		    else/*we are in all or part of the \r\n end of chunk data*/
			 req->write_to_module_pending=0;
		    req->chunk_bytes_read+=remains;
		    req->pstrblock_read+=remains;
		    req->pstrblock_read_len-=remains;
	       }else{
		    tmp=remains-req->pstrblock_read_len;
		    if(tmp<2)
			 req->write_to_module_pending=req->pstrblock_read_len-tmp;
		    else
			 req->write_to_module_pending=req->pstrblock_read_len;
		    req->chunk_bytes_read+=req->pstrblock_read_len;
		    req->pstrblock_read+=req->pstrblock_read_len;
		    req->pstrblock_read_len-=req->pstrblock_read_len;		   
	       }
	 }

	  if(req->pstrblock_read_len==0)
	       return CI_NEEDS_MORE;

    }
    return CI_OK;
}

int chunks_read(request_t *req){
    int bytes;
  
    if(req->pstrblock_read!=req->rbuf){
	/*... put the current data to the begining of buf ....*/
	 if(req->pstrblock_read_len)
	      memmove(req->rbuf,req->pstrblock_read,req->pstrblock_read_len);
	req->pstrblock_read=req->rbuf;
    }
    
    bytes=BUFSIZE-req->pstrblock_read_len;
    if(bytes<=0){
	ci_debug_printf(5,"Not enough space to read data! is this a bug (%d %d)?????\n",
			req->pstrblock_read_len, BUFSIZE);
	return CI_ERROR;
    }

    if((bytes=ci_read(req->connection->fd,
		      req->rbuf+req->pstrblock_read_len,
		      bytes,TIMEOUT))<0){ /*... read some data...*/
	ci_debug_printf(5,"Error reading data (read return=%d) \n",bytes);
	return CI_ERROR; 
    }
    req->pstrblock_read_len+=bytes;/* ... (size of data is readed plus old )...*/
    return CI_OK;
}


int get_options(request_t *req,char *icap_server,char *service){
    
    if(CI_OK!=create_request(req,icap_server,service,ICAP_OPTIONS))
	return CI_ERROR;
    ci_send_request_headers(req,0);
    reset_header(req->head);
    ci_read_icap_header(req,req->head,128);
    ci_headers_unpack(req->head);
    ci_get_request_options(req,req->head);

    return CI_OK;
}



char *icap_server="localhost";
char *service="echo";
char *input_file=NULL;
char *output_file=NULL;
int RESPMOD=1;

static struct options_entry options[]={
     {"-i","icap_servername",&icap_server,ci_cfg_set_str,"The icap server name"},
     {"-f","filename",&input_file,ci_cfg_set_str,"Send this file to the icap server.\nDefault is to send an options request"},
     {"-o","filename",&output_file,ci_cfg_set_str,"Save output to this file.\nDefault is to send to the stdout"},
     {"-req",NULL,&RESPMOD,ci_cfg_disable,"Send a request modification instead of responce modification"},
     {"-d","level", &CI_DEBUG_LEVEL,ci_cfg_enable,"Print debug info to stdout"},
     {NULL,NULL,NULL,NULL}
};

void log_errors(request_t *req, const char *format,... ){
     va_list ap;
     va_start(ap,format);
     vfprintf(stderr,format,ap);
     va_end(ap);
}

void vlog_errors(request_t *req, const char *format, va_list ap){
    vfprintf(stderr,format,ap);
}

int ci_host_to_sockaddr_t(char *servername,ci_sockaddr_t *addr,int proto){
    int ret=0;
    struct addrinfo hints,*res;
    memset(&hints,0,sizeof(hints));
    hints.ai_family=proto;
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_protocol=0;
    if((ret=getaddrinfo(servername,NULL,&hints,&res))!=0){
	ci_debug_printf(1,"Error geting addrinfo return was:%d\n",ret);
	return 0;
    }
    //fill the addr..... and 
    memcpy(&(addr->sockaddr),res->ai_addr,CI_SOCKADDR_SIZE);
    freeaddrinfo(res);
    return 1;
}


ci_connection_t *ci_connect_to(char *servername,int port,int proto){
    ci_connection_t *connection=malloc(sizeof(ci_connection_t));
    char hostname[CI_MAXHOSTNAMELEN+1];
    int addrlen=0;
    if(!connection)
	return NULL;
    connection->fd = socket(proto, SOCK_STREAM, 0);
    if(connection->fd == -1){
	ci_debug_printf(1,"Error oppening socket ....\n");
	free(connection);
	return NULL;
    }

    if(!ci_host_to_sockaddr_t(servername,&(connection->srvaddr),proto)){
	free(connection);
	return NULL;	
    }
    ci_sockaddr_set_port(&(connection->srvaddr),port);

    if(connect(connection->fd, (struct sockaddr *)&(connection->srvaddr.sockaddr), CI_SOCKADDR_SIZE)){
	ci_sockaddr_t_to_host(&(connection->srvaddr), hostname, CI_MAXHOSTNAMELEN);
	ci_debug_printf(1,"Error connecting to socket (host: %s) .....\n",hostname);
	free(connection);
	return NULL;
    }
     
    addrlen=CI_SOCKADDR_SIZE;
    getsockname(connection->fd,(struct sockaddr *)&(connection->claddr.sockaddr),&addrlen);
    ci_fill_sockaddr(&(connection->claddr));
    ci_fill_sockaddr(&(connection->srvaddr));
    
    ci_netio_init(connection->fd);
    return connection;
}


int prepere_body_chunk(request_t *req,void *data, int (*readdata)(void *data,char *,int,struct request *)){
     int chunksize,def_bytes;
     char *wbuf=NULL;
     char tmpbuf[EXTRA_CHUNK_SIZE];

     
     wbuf=req->wbuf+EXTRA_CHUNK_SIZE;/*Let size of EXTRA_CHUNK_SIZE space in the beggining of chunk*/
     if((chunksize=(*readdata)(data,wbuf,MAX_CHUNK_SIZE,req)) <=0){
/*	  ci_debug_printf(1,"No data to send or eof reached (%d,).......\n",chunksize);*/
	  req->remain_send_block_bytes=0;
	  return chunksize; /*Must be 0 or CI_EOF */
     }

     wbuf+=chunksize; /*Put the "\r\n" sequence at the end of chunk*/
     *(wbuf++)='\r';
     *wbuf='\n';

     def_bytes=snprintf(tmpbuf,EXTRA_CHUNK_SIZE,"%x\r\n",chunksize);
     wbuf=req->wbuf+EXTRA_CHUNK_SIZE-def_bytes;  /*Copy the chunk define in the beggining of chunk .....*/
     memcpy(wbuf,tmpbuf,def_bytes);
     
     req->pstrblock_responce=wbuf;
     req->remain_send_block_bytes=def_bytes+chunksize+2;     

     return req->remain_send_block_bytes;
}


int send_get_data(request_t *req,void *data_source,int (*source_read)(void *,char *,int,request_t *)){
     int ret,v1,v2,i,status;
     char *buf,*val;

     while( prepere_body_chunk(req,data_source,source_read)>0){
	  ret=ci_writen(req->connection->fd,req->pstrblock_responce,req->remain_send_block_bytes,TIMEOUT);
     }
     ret=ci_writen(req->connection->fd,"0\r\n\r\n",5,TIMEOUT);
     printf("OK sending body\n");
     
     /*Reseting responce headers*/
     reset_header(req->head);
     /*And reading the new .....*/
     ci_read_icap_header(req,req->head,TIMEOUT);
     ci_headers_unpack(req->head);
     sscanf(req->head->buf,"ICAP/%d.%d %d",&v1,&v2,&status);
     ci_debug_printf(1,"Responce was with status:%d \n",status);
     /*read encups headers */
     
     if((val=search_header(req->head,"Encapsulated"))==NULL){
	  ci_debug_printf(1,"No encapsulated entities!\n");
	  return CI_OK;
     }
     process_encapsulated(req,val);
     
     
     ret=read_encaps_headers(req);
     ci_debug_printf(5,"Read_encaps_headers return:%d\n",ret);
     /*read the rest body .......*/
     
     ci_debug_printf(5,"OK reading headers\nBody READ:\n");
     
     req->current_chunk_len=0;
     req->chunk_bytes_read=0;
     req->write_to_module_pending=0;
     while((ret=chunks_read(req))!=CI_ERROR){
	  do{
	       if((ret=parse_chunk_data(req,&buf))==CI_ERROR){
		    ci_debug_printf(1,"Error parsing chunks, current chunk len: %d readed:%d, str:%s\n",
				    req->current_chunk_len,
				    req->chunk_bytes_read,
				    req->pstrblock_read);
		    return CI_ERROR;
	       }
//	       for(i=0;i<req->write_to_module_pending;i++)
//		    printf("%c",buf[i]);
	       req->write_to_module_pending=0;
	       if(ret==CI_EOF)
		    return CI_OK;
	  }while(ret!=CI_NEEDS_MORE);
     }

     if(ret==CI_ERROR)
	  return CI_ERROR;

     return CI_OK;
}


int do_send_file_request(request_t *req,char *icap_server,char *service,void *data_source,int (*source_read)(void *,char *,int,request_t *)){
    int ret,v1,v2,remains,pre_eof=0,preview_status;
    char *buf;
    
    if(CI_OK!=create_request(req,icap_server,service,ICAP_RESPMOD)){
	ci_debug_printf(1,"Error making respmod request ....\n");
	return CI_ERROR;
    }

    if(req->preview>0){ /*The preview data will be send with headers....*/
	ci_buf_mem_alloc(&(req->preview_data),req->preview); /*Alloc mem for preview data*/
	buf=req->preview_data.buf;
	remains=req->preview;
	while(remains && ! pre_eof){  /*Getting the preview data*/
	    if((ret=(*source_read)(data_source,buf,remains,req))<=0){
		pre_eof=1;
		break;
	    }
	    remains-=ret;
	    
	}
	req->preview-=remains;
	req->preview_data.used=req->preview;
    }

    /*Adding the */
    ci_request_create_respmod(req,1);
    ci_respmod_add_header(req,"Filetype: Unknown");
    ci_respmod_add_header(req,"User: chtsanti");
    
    printf("Going to send what we have\n");
    ci_send_request_headers(req,pre_eof);
    printf("OK sending headers\n");
    /*send body*/

    reset_header(req->head);
    preview_status=100;
    if(!pre_eof){
	
	if(req->preview>0){/*we must wait for ICAP responce here.....*/
	    ci_read_icap_header(req,req->head,TIMEOUT);
	    ci_debug_printf(1,"I am getting preview responce %s\n",req->head->buf);

	    sscanf(req->head->buf,"ICAP/%d.%d %d",&v1,&v2,&preview_status);
	    ci_debug_printf(1,"Responce was with status:%d \n",preview_status);
	}
	
    }

    if(preview_status!=204){
	 send_get_data(req,data_source,source_read);
    }

    return CI_OK;    
}



int fileread(void *fd,char *buf,int len,request_t *req){
    int ret;
    ret=read(*(int *)fd,buf,len);
    return ret;
}


int main(int argc, char **argv){
     int fd_in;
     int i,port=1344;
     ci_connection_t *conn;
     request_t *req;
     int preview,keepalive,allow204;

     CI_DEBUG_LEVEL=10; /*Default debug level is 1*/

     if(!ci_args_apply(argc,argv,options)){
	 ci_args_usage(argv[0],options);
	 exit(-1);
     }

#if ! defined(_WIN32)
     __log_error=(void(*)(void *, const char *,... ))log_errors; /*set c-icap library log  function*/
#else
     __vlog_error=vlog_errors; /*set c-icap library  log function for win32.....*/
#endif


     if(!(conn=ci_connect_to(icap_server,port,AF_INET))){
	 ci_debug_printf(1,"Failed to connect to icap server.....\n");
	 exit(-1);
     }

     req=ci_request_alloc(conn);

     get_options(req,icap_server,service);
     ci_debug_printf(1,"OK done with options!\n");

     if(!input_file){
	 printf("OPTIONS:\n");
	 printf("\tAllow 204:%s\n\tPreview:%d\n\tKeep alive:%s\n",
		(req->allow204?"Yes":"No"),
		req->preview,
		(req->keepalive?"Yes":"No")
	     );
	 printf("HEADERS:\n");
	 for(i=0;i<req->head->used;i++){
	     printf("\t%s\n",req->head->headers[i]);
	 }
     }
     else{
	 if((fd_in=open(input_file,O_RDONLY))<0){
	     ci_debug_printf(1,"Error openning file %s\n",input_file);
	     exit(-1);
	 }
	 
	 preview=req->preview;
	 keepalive=req->keepalive;
	 allow204=req->allow204;;
	 
	 ci_request_reset(req);

	 req->preview=preview;
	 req->keepalive=keepalive;
	 req->allow204=allow204;
	 ci_debug_printf(1,"Preview:%d keepalive:%d,allow204:%d\n",
			 preview,keepalive,allow204);

	 ci_debug_printf(1,"OK allocating request going to send request\n");
	 do_send_file_request(req,icap_server,service,&fd_in,(int (*)(void *,char *,int,request_t *))fileread);
	 close(fd_in);
	 ci_debug_printf(1, "Done\n");
     }
     close(conn->fd);      
     return 0;
}
