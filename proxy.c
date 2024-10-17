#include <stdio.h>
#include "csapp.h"
#include "sbuf.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_ENTRIES 10
#define SBUF_SIZE 16
#define NUM_THREADS 4

sbuf_t connection_buffer; // 连接缓冲区

// URI结构
struct UriInfo
{
    char hostname[MAXLINE]; // 主机名
    char port[MAXLINE];     // 端口
    char path[MAXLINE];     // 路径
};

// 缓存块结构
typedef struct
{
    char content[MAX_OBJECT_SIZE];
    char url[MAXLINE];
    int lru_counter;
    int is_empty;

    int reader_count; // 读者数量
    sem_t write_lock; // 保护缓存块
    sem_t mutex;      // 保护 reader_count
} CacheBlock;

typedef struct
{
    CacheBlock entries[MAX_CACHE_ENTRIES];
    int count;
} Cache;

Cache web_cache;

void handle_connection(int client_fd);
void parse_uri(char* uri, struct UriInfo* uri_data);
void build_request_header(char* http_header, struct UriInfo* uri_data, rio_t* client_rio);
void* worker_thread(void* vargp);

void initialize_cache();
int find_in_cache(char* url);
int get_free_index();
void update_lru(int index);
void write_to_cache(char* url, char* content);

/* 处理SIGPIPE信号 */
void sigpipe_handler(int sig)
{
    printf("Caught SIGPIPE\n");
}

int main(int argc, char** argv)
{
    int listen_fd, client_fd;
    socklen_t client_len;
    char host_name[MAXLINE], port_num[MAXLINE];

    struct sockaddr_storage client_addr;
    initialize_cache();
    pthread_t thread_id;

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    signal(SIGPIPE, sigpipe_handler);
    listen_fd = Open_listenfd(argv[1]);

    sbuf_init(&connection_buffer, SBUF_SIZE);
    // 创建工作者线程
    for (int i = 0; i < NUM_THREADS; i++)
    {
        Pthread_create(&thread_id, NULL, worker_thread, NULL);
    }

    while (1)
    {
        client_len = sizeof(client_addr);
        client_fd = Accept(listen_fd, (SA*)&client_addr, &client_len);
        sbuf_insert(&connection_buffer, client_fd);
        Getnameinfo((SA*)&client_addr, client_len, host_name, MAXLINE, port_num, MAXLINE, 0);
        printf("Accepted connection from (%s, %s).\n", host_name, port_num);
    }
    return 0;
}

void* worker_thread(void* vargp)
{
    Pthread_detach(pthread_self());
    while (1)
    {
        int client_fd = sbuf_remove(&connection_buffer);
        handle_connection(client_fd);
        Close(client_fd);
    }
}

void handle_connection(int client_fd)
{
    char buffer[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char server[MAXLINE];

    rio_t client_rio, server_rio;

    char cache_key[MAXLINE];
    Rio_readinitb(&client_rio, client_fd);
    Rio_readlineb(&client_rio, buffer, MAXLINE);
    sscanf(buffer, "%s %s %s", method, uri, version);
    strcpy(cache_key, uri);

    if (strcasecmp(method, "GET"))
    {
        printf("Proxy does not implement the method\n");
        return;
    }

    struct UriInfo* uri_data = (struct UriInfo*)malloc(sizeof(struct UriInfo));
    // 判断URI是否在缓存中，如果在缓存中，直接回复
    int index;
    if ((index = find_in_cache(cache_key)) != -1)
    {
        // 加锁
        P(&web_cache.entries[index].mutex);
        web_cache.entries[index].reader_count++;
        if (web_cache.entries[index].reader_count == 1)
            P(&web_cache.entries[index].write_lock);
        V(&web_cache.entries[index].mutex);

        Rio_writen(client_fd, web_cache.entries[index].content, strlen(web_cache.entries[index].content));

        P(&web_cache.entries[index].mutex);
        web_cache.entries[index].reader_count--;
        if (web_cache.entries[index].reader_count == 0)
            V(&web_cache.entries[index].write_lock);
        V(&web_cache.entries[index].mutex);
        free(uri_data);
        return;
    }

    // 解析URI
    parse_uri(uri, uri_data);

    // 构建请求头
    build_request_header(server, uri_data, &client_rio);

    // 连接到服务器
    int server_fd = Open_clientfd(uri_data->hostname, uri_data->port);
    if (server_fd < 0)
    {
        printf("Connection failed\n");
        free(uri_data);
        return;
    }

    Rio_readinitb(&server_rio, server_fd);
    Rio_writen(server_fd, server, strlen(server));

    char cache_content[MAX_OBJECT_SIZE];
    int content_size = 0;
    size_t bytes_read;
    while ((bytes_read = Rio_readlineb(&server_rio, buffer, MAXLINE)) != 0)
    {
        // 注意判断是否会超出缓存大小
        content_size += bytes_read;
        if (content_size < MAX_OBJECT_SIZE)
            strcat(cache_content, buffer);
        printf("Proxy received %d bytes, then sent\n", (int)bytes_read);
        Rio_writen(client_fd, buffer, bytes_read);
    }
    Close(server_fd);

    if (content_size < MAX_OBJECT_SIZE)
    {
        write_to_cache(cache_key, cache_content);
    }
    free(uri_data);
}

void build_request_header(char* http_header, struct UriInfo* uri_data, rio_t* client_rio)
{
    char* user_agent = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
    char* conn_hdr = "Connection: close\r\n";
    char* prox_hdr = "Proxy-Connection: close\r\n";
    char* host_hdr_format = "Host: %s\r\n";
    char* request_hdr_format = "GET %s HTTP/1.0\r\n";
    char* end_of_hdr = "\r\n";

    char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
    sprintf(request_hdr, request_hdr_format, uri_data->path);
    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0)
    {
        if (strcmp(buf, end_of_hdr) == 0)
            break; /*EOF*/

        if (!strncasecmp(buf, "Host", strlen("Host"))) /*Host:*/
        {
            strcpy(host_hdr, buf);
            continue;
        }

        if (!strncasecmp(buf, "Connection", strlen("Connection")) && !strncasecmp(buf, "Proxy-Connection", strlen("Proxy-Connection")) && !strncasecmp(buf, "User-Agent", strlen("User-Agent")))
        {
            strcat(other_hdr, buf);
        }
    }
    if (strlen(host_hdr) == 0)
    {
        sprintf(host_hdr, host_hdr_format, uri_data->hostname);
    }
    sprintf(http_header, "%s%s%s%s%s%s%s",
        request_hdr,
        host_hdr,
        conn_hdr,
        prox_hdr,
        user_agent,
        other_hdr,
        end_of_hdr);

    return;
}

// 解析URI
void parse_uri(char* uri, struct UriInfo* uri_data)
{
    char* host_start = strstr(uri, "//");
    if (host_start == NULL)
    {
        char* path_start = strstr(uri, "/");
        if (path_start != NULL)
            strcpy(uri_data->path, path_start);
        strcpy(uri_data->port, "80");
        return;
    }
    else
    {
        char* port_start = strstr(host_start + 2, ":");
        if (port_start != NULL)
        {
            int tmp;
            sscanf(port_start + 1, "%d%s", &tmp, uri_data->path);
            sprintf(uri_data->port, "%d", tmp);
            *port_start = '\0';
        }
        else
        {
            char* path_start = strstr(host_start + 2, "/");
            if (path_start != NULL)
            {
                strcpy(uri_data->path, path_start);
                strcpy(uri_data->port, "80");
                *path_start = '\0';
            }
        }
        strcpy(uri_data->hostname, host_start + 2);
    }
    return;
}

// 初始化缓存
void initialize_cache()
{
    web_cache.count = 0;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++)
    {
        web_cache.entries[i].lru_counter = 0;
        web_cache.entries[i].is_empty = 1;
        Sem_init(&web_cache.entries[i].write_lock, 0, 1);
        Sem_init(&web_cache.entries[i].mutex, 0, 1);
        web_cache.entries[i].reader_count = 0;
    }
}

// 从缓存中查找内容
int find_in_cache(char* url)
{
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++)
    {
        P(&web_cache.entries[i].mutex);
        web_cache.entries[i].reader_count++;
        if (web_cache.entries[i].reader_count == 1)
            P(&web_cache.entries[i].write_lock);
        V(&web_cache.entries[i].mutex);

        if (web_cache.entries[i].is_empty == 0 && strcmp(url, web_cache.entries[i].url) == 0)
        {
            P(&web_cache.entries[i].mutex);
            web_cache.entries[i].reader_count--;
            if (web_cache.entries[i].reader_count == 0)
                V(&web_cache.entries[i].write_lock);
            V(&web_cache.entries[i].mutex);
            return i;
        }

        P(&web_cache.entries[i].mutex);
        web_cache.entries[i].reader_count--;
        if (web_cache.entries[i].reader_count == 0)
            V(&web_cache.entries[i].write_lock);
        V(&web_cache.entries[i].mutex);
    }
    return -1;
}

// 找到可以存放的缓存行
int get_free_index()
{
    int min_lru = __INT_MAX__;
    int min_index = 0;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++)
    {
        P(&web_cache.entries[i].mutex);
        web_cache.entries[i].reader_count++;
        if (web_cache.entries[i].reader_count == 1)
            P(&web_cache.entries[i].write_lock);
        V(&web_cache.entries[i].mutex);

        if (web_cache.entries[i].is_empty == 1)
        {
            min_index = i;
            P(&web_cache.entries[i].mutex);
            web_cache.entries[i].reader_count--;
            if (web_cache.entries[i].reader_count == 0)
                V(&web_cache.entries[i].write_lock);
            V(&web_cache.entries[i].mutex);
            break;
        }
        if (web_cache.entries[i].lru_counter < min_lru)
        {
            min_index = i;
            min_lru = web_cache.entries[i].lru_counter;
        }

        P(&web_cache.entries[i].mutex);
        web_cache.entries[i].reader_count--;
        if (web_cache.entries[i].reader_count == 0)
            V(&web_cache.entries[i].write_lock);
        V(&web_cache.entries[i].mutex);
    }

    return min_index;
}

// 更新LRU
void update_lru(int index)
{
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++)
    {
        if (web_cache.entries[i].is_empty == 0 && i != index)
        {
            P(&web_cache.entries[i].write_lock);
            web_cache.entries[i].lru_counter--;
            V(&web_cache.entries[i].write_lock);
        }
    }
}

// 写入缓存
void write_to_cache(char* url, char* content)
{
    int index = get_free_index();
    P(&web_cache.entries[index].write_lock);
    strcpy(web_cache.entries[index].content, content);
    strcpy(web_cache.entries[index].url, url);
    web_cache.entries[index].is_empty = 0;
    web_cache.entries[index].lru_counter = __INT_MAX__;
    update_lru(index);
    V(&web_cache.entries[index].write_lock);
}
