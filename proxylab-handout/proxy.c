#include <stdio.h>
#include <string.h>
#include "csapp.h"

/* Recommended max Cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

// data structure
typedef struct 
{
  char host[MAXLINE];
  char port[MAXLINE];
  char query[MAXLINE];
} Rqst_line;

typedef struct
{
  char key[MAXLINE];
  char val[MAXLINE];
} Rqst_header;

typedef struct 
{
  char *id;
  char *object;
} Cache_line;

typedef struct 
{
  int resident_cnt;
  Cache_line *cache_lines;
} Cache;


// global shard variable;
Cache cache;
volatile int readcnt;
sem_t mutex, w;

// function prototype
void *thread(void *vargp);
void asServer(int serverfd);
void parse_request(int serverfd, rio_t *serverRio, Rqst_line *line, Rqst_header *headers, int *num_hdrs);
void parse_uri(char *uri, Rqst_line *line);
Rqst_header parse_header(char *line);
void send_request(int clientfd, rio_t *serverRio, Rqst_line *line, Rqst_header *headers, int num_hdrs);
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg);
void init_cache();
void init_sem();
void delete_cache();
int reader(int serverfd, char *uri);
void writer(char *uri, char *buf);

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

// robustness: long-running processes and web proxies are no exception.
// what kinds of types of errors should be considered
// how to react, should not immediately exit.

// client and server
int main(int argc, char **argv)
{
	int listenfd, *serverfd;
	socklen_t clientlen;
	struct sockaddr_storage clientaddr;  // Enough space for any address
	char client_hostname[MAXLINE], client_port[MAXLINE];

  pthread_t tid;

	if(argc != 2){
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(0);
	}
	
  init_cache();
  init_sem();

  listenfd = Open_listenfd(argv[1]);	
  while(1){
		clientlen = sizeof(struct sockaddr_storage);
    serverfd = Malloc(sizeof(int)); 
    // independent memory block to avoid race
		*serverfd = Accept(listenfd, (SA*)&clientaddr, &clientlen);
		Getnameinfo((SA*)&clientaddr, clientlen, client_hostname, MAXLINE,
				client_port, MAXLINE, 0);
		printf("Proxy connected to (%s, %s)\n", client_hostname, client_port);
    Pthread_create(&tid, NULL, thread, serverfd);
	}
  printf("%s", user_agent_hdr);
  delete_cache();
  return 0;
}

void *thread(void *vargp)
{
  // detach itself to enable OS to collect the resources at the end of life
  int serverfd = *((int *)vargp);
  Pthread_detach(Pthread_self());
  Free(vargp);
  asServer(serverfd);
  Close(serverfd);
  return NULL;
}


void asServer(int serverfd)
{
  int clientfd=0;
  int num_hdrs;
  char buf[MAXLINE];
  char uri[MAXLINE];
  Rqst_line line;
  Rqst_header headers[20];
  rio_t serverRio, clientRio;
  Rio_readinitb(&serverRio, serverfd);

	// Parse request
  parse_request(serverfd, &serverRio, &line, headers, &num_hdrs);
	
  // see if in the cache
  strcpy(uri, line.host);
  strcpy(uri+strlen(uri), line.query);
  if (reader(serverfd, uri)) 
  {
      fprintf(stdout, "%s from cache\n", uri);
      fflush(stdout);
      return;
  }

	// proxy connect the server as client
	clientfd = Open_clientfd(line.host, line.port);
	Rio_readinitb(&clientRio, clientfd);

	// send the request to server
	send_request(clientfd, &serverRio, &line, headers, num_hdrs);

  // high-risky buggy code
  int n;
  int tot_size;
  char line_buf[MAX_OBJECT_SIZE];
  tot_size=0;
	while((n=Rio_readlineb(&clientRio, buf, MAXLINE)))
  {
		Rio_writen(serverfd, buf, n);
		printf("%s", buf);
    strcpy(line_buf+tot_size,buf);
    tot_size+=n;
	}
  writer(uri,line_buf);
	Close(clientfd);
}

void parse_request
(int serverfd, rio_t *serverRio, Rqst_line *line, Rqst_header *headers, int *num_hdrs)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];

    Rio_readinitb(serverRio, serverfd);
    if (!Rio_readlineb(serverRio, buf, MAXLINE))  
        return;
    
    //parse request line
    sscanf(buf, "%s %s %s", method, uri, version);
    parse_uri(uri, line);
    *num_hdrs = 0;
    Rio_readlineb(serverRio, buf, MAXLINE);
    while(strcmp(buf, "\r\n")) {
      headers[(*num_hdrs)++] = parse_header(buf);
      Rio_readlineb(serverRio, buf, MAXLINE);
    }
}




void parse_uri(char *uri, Rqst_line *line)
{
	char *afterTwoSlash, *thirdSlash, *colon;
	afterTwoSlash = strstr(uri, "/");
	afterTwoSlash+=2; // skip the first two /
	thirdSlash = strstr(afterTwoSlash, "/");
	colon = strstr(afterTwoSlash, ":");
	strcpy(line->query, thirdSlash);
	if(colon != NULL)
	{
		int nameLen = colon - afterTwoSlash;
		strncpy(line->host, afterTwoSlash, nameLen);
		line->host[nameLen]='\0';
		int portLen = thirdSlash-(colon+1);
		strncpy(line->port, colon+1, portLen);
		line->port[portLen]='\0';
	}
	else
	{
		int nameLen = thirdSlash - afterTwoSlash;
		strncpy(line->host, afterTwoSlash, nameLen);
		line->host[nameLen]='\0';
    strcpy(line->port,"80");
	}
}


Rqst_header parse_header(char *line) {
    Rqst_header header;
    char *c = strstr(line, ": ");
    if (c == NULL) {
        fprintf(stderr, "Error: invalid header: %s\n", line);
        exit(0);
    }
    *c = '\0';
    strcpy(header.key, line);
    strcpy(header.val, c+2);
    return header;
}

void send_request(int clientfd, rio_t *serverRio, Rqst_line *line, Rqst_header *headers, int num_hdrs)
{
  char buf[MAXLINE], *buf_head = buf;
  sprintf(buf_head, "GET %s HTTP/1.0\r\n", line->query);
  printf("> %s", buf_head);
  buf_head = buf + strlen(buf);
  for (int i = 0; i < num_hdrs; ++i) {
        sprintf(buf_head, "%s: %s", headers[i].key, headers[i].val);
        printf("> %s, len: %d", buf_head, (int)strlen(buf_head));
        buf_head = buf + strlen(buf);
    }
  sprintf(buf_head, "\r\n");
  printf("> %s, len: %d", buf_head, (int)strlen(buf_head));
  Rio_writen(clientfd, buf, MAXLINE);
}

/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
/* $end clienterror */


int reader(int serverfd, char *uri) {
    int hit= 0;
    P(&mutex);
    readcnt++;
    if (readcnt == 1) {
        P(&w);
    }
    V(&mutex);

    for (int i = 0; i < 10; ++i) {
        if (!strcmp(cache.cache_lines[i].id, uri)) {
            Rio_writen(serverfd, cache.cache_lines[i].object, MAX_OBJECT_SIZE);
            hit = 1;
            break;
        }
    }
    P(&mutex);
    readcnt--;
    if (readcnt == 0) {
        V(&w);
    }
    V(&mutex);
    return hit;
}

void writer(char *uri, char *buf) {
    P(&w);
    strcpy(cache.cache_lines[cache.resident_cnt].id, uri);
    strcpy(cache.cache_lines[cache.resident_cnt].object, buf);
    ++cache.resident_cnt;
    V(&w);
}


void init_cache()
{
  cache.resident_cnt=0;
  cache.cache_lines=(Cache_line*)Malloc(sizeof(char)*MAX_CACHE_SIZE);
  for(int i=0;i<10;i++)
  {
    cache.cache_lines[i].id = (char*)Malloc(sizeof(char)*MAXLINE);
    cache.cache_lines[i].object=(char*)Malloc(sizeof(char)*MAX_OBJECT_SIZE);
  }
}

void delete_cache()
{
  for(int i=0;i<10;i++)
  {
    Free(cache.cache_lines[i].id);
    Free(cache.cache_lines[i].object);
  }
  Free(cache.cache_lines);
}

void init_sem()
{
  readcnt=0;
  Sem_init(&mutex, 0, 1);
  Sem_init(&w, 0, 1);
}