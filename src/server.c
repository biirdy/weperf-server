#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>

#include <syslog.h>
#include <unistd.h>
#include <signal.h>

#include <mysql.h>

#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include <pthread.h>

#include <xmlrpc-c/base.h>
#include <xmlrpc-c/server.h>
#include <xmlrpc-c/server_abyss.h>

#include "srrp.h"
#include "ini/ini.h"
#include "ini/ini.c"

MYSQL *conn;
int deamon = 0;
char recv_buff[1024];
char send_buff[1024];

struct in_addr server_ip;       

/*
* Config struct to be loaded in 
*/
typedef struct{
    //rcp
    int server_rpc_port;

    //database
    const char* mysql_addr;
    const char* mysql_usr;
    const char* mysql_pass;

    //server
    const char* server_addr;
    const char* server_pub_addr;
    const char* server_ether;
    int server_port;
    int server_timeout;

    //iperf
    int tcp_iperf_port;
    int udp_iperf_port;

	//interval
	int rtt_interval;
	int tcp_bw_interval;
	int dns_interval;
} configuration;
configuration config;


static int handler(void* user, const char* section, const char* name, const char* value){
    configuration* pconfig = (configuration*)user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("rpc", "server_rpc_port")) {
        pconfig->server_rpc_port = atoi(value);
    }else if (MATCH("database", "mysql_addr")){
        pconfig->mysql_addr = strdup(value);
    }else if (MATCH("database", "mysql_usr")){
        pconfig->mysql_usr = strdup(value);
    }else if (MATCH("database", "mysql_pass")){
        pconfig->mysql_pass = strdup(value);
    }else if (MATCH("server", "server_addr")){
        pconfig->server_addr = strdup(value);
    }else if (MATCH("server", "server_pub_addr")){
        pconfig->server_pub_addr = strdup(value);
    }else if (MATCH("server", "server_port")){
        pconfig->server_port = atoi(value);
    }else if (MATCH("server", "server_timeout")){
        pconfig->server_timeout = atoi(value);
    }else if (MATCH("server", "server_ether")){
        pconfig->server_ether = strdup(value);
    }else if (MATCH("iperf", "tcp_iperf_port")){
        pconfig->tcp_iperf_port = atoi(value);
    }else if (MATCH("iperf", "udp_iperf_port")){
        pconfig->udp_iperf_port = atoi(value);
    }else if (MATCH("interval", "rtt_interval")){
        pconfig->rtt_interval = atoi(value);
    }else if (MATCH("interval", "tcp_bw_interval")){
        pconfig->tcp_bw_interval = atoi(value);
    }else if (MATCH("interval", "dns_interval")){
        pconfig->dns_interval = atoi(value);
    }else {
        return 0;  /* unknown section/name, error */
    }
    return 1;
}

/*
* Dictionary to store id to socket 
* http://stackoverflow.com/questions/4384359/quick-way-to-implement-dictionary-in-c
*/
struct nlist { /* table entry: */
    struct nlist *next; /* next entry in chain */
    int id;     /* defined name */
    int socket; /* replacement text */
};

/*
* Dictionary to store id to ip(in_addr)
*/
struct iplist{
    struct iplist *next;
    int id;
    struct in_addr ip;
};

void rotate_name(const char *oname, char *nname){
    const char  *ptr;
    int         number, plen;

    for(ptr = oname; *ptr && !isdigit(*ptr); ptr++)
            ;
    if(*ptr == '\0')
            number = 0;
    else
            number = atoi(ptr);
    plen = ptr - oname;
    if(plen <= 0)
            return;
    memcpy(nname, oname, plen);
    sprintf(nname + plen, "%d", number + 1);
}

void server_log(const char * type, const char * fmt, ...){

    struct stat st;
    stat("/var/log/weperf/server/server.log", &st);
    int size = st.st_size;

    //increment all log file versions
    if(size > 1024 * 1024){
        
        DIR *dp;
        struct dirent *ep;
        char * dir = "/var/log/weperf/server";     
        dp = opendir (dir);

        if (dp != NULL){
            while (ep = readdir (dp)){
                char nname[100];
                char odir[100];
                char ndir[100];
                rotate_name(ep->d_name, nname);
                sprintf(odir, "%s%s", dir, (char *) ep->d_name);
                sprintf(ndir, "%s%s", dir, nname);
                rename(odir, ndir);
            }
            (void) closedir (dp);
        }
    }

    //log files
    FILE* logs = fopen("/var/log/weperf/server/server.log", "a");

    //format message
    va_list args; 
    va_start(args, fmt);

    char msg[100];
    vsprintf(msg, fmt, args);

    va_end(args);

    //get timestamp
    time_t ltime;
    struct tm result;
    char stime[32];
    ltime = time(NULL);
    localtime_r(&ltime, &result);
    asctime_r(&result, stime);
    strtok(stime, "\n");            

    fprintf(logs, "%s - Server - [%s] - %s\n", stime, type, msg);       //write to log
    fflush(logs);

    // if not deamon - print logs
    if(!deamon)
        printf("%s - %s\n", type, msg);

    fclose(logs);
}

#define HASHSIZE 101

static struct nlist * hashtab[HASHSIZE];
static struct iplist * iphashtab[HASHSIZE];

/* hash: form hash value for string s */
unsigned hash(int id){
    return id % HASHSIZE;
}

/* lookup: look for s in hashtab */
struct nlist *lookup(int id){
    struct nlist *np;

    for (np = hashtab[hash(id)]; np != NULL; np = np->next){
        if (id == np->id){
            return np; /* found */
        }
    }

    return NULL; /* not found */
}

struct iplist * iplookup(int id){
    struct iplist * ipl;

    for (ipl = iphashtab[hash(id)]; ipl != NULL; ipl = ipl->next){
        if (id == ipl->id){
            return ipl; /* found */
        }
    }

    return NULL; /* not found */
}

/* install: put (name, defn) in hashtab */
struct nlist *install(int id, int socket){
    struct nlist *np;
    unsigned hashval;
    if ((np = lookup(id)) == NULL) { /* not found */
        np = (struct nlist *) malloc(sizeof(*np));
        np->id = id;
        hashval = hash(id);
        np->next = hashtab[hashval];
        hashtab[hashval] = np;

        printf("Installing socket with hashval %d\n", hash(id));
    }
    np->socket = socket;
}

struct iplist * ipinstall(int id, struct in_addr ip){
    struct iplist *ipl;
    unsigned hashval;
    if ((ipl = iplookup(id)) == NULL) { /* not found */
        ipl = (struct iplist *) malloc(sizeof(*ipl));
        ipl->id = id;
        hashval = hash(id);
        ipl->next = iphashtab[hashval];
        iphashtab[hashval] = ipl;

        printf("Installing ip with hashval %d\n", hash(id));
    }
    iphashtab[hash(id)]->ip = ip;
}



static xmlrpc_value * iperf_request(xmlrpc_env *   const envP,
           xmlrpc_value * const paramArrayP,
           void *         const serverInfo,
           void *         const channelInfo) {

    xmlrpc_int32 id, dst_id, m_id, length;

    //parse argument array
    xmlrpc_decompose_value(envP, paramArrayP, "(iiii)", &id, &dst_id, &m_id, &length);
    if (envP->fault_occurred){
        server_log("Error", "Could not parse iperf request argument array");
        return NULL;
    }

    //get socket
    struct nlist * sck = lookup((int) id);

    //get dst ip
    struct iplist * dst_ip = iplookup((int) dst_id);

    if(sck != NULL && dst_ip != NULL){

        //create request
        request_init((struct srrp_request *) send_buff, SRRP_BW, (int) dst_id, dst_ip->ip, (int) m_id); 
        add_param((struct srrp_request *) send_buff, SRRP_DUR, (int) length);

        //send
        int send_result = send(sck->socket, send_buff, request_size((struct srrp_request *) send_buff), 0);

        //check if send was successful - socket has probably been closed
        if(send_result == request_size((struct srrp_request *) send_buff)){
            server_log("Info", "RPC sending iperf request to sensor %d", id);
            return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 0);    
        }else{
            server_log("Error", "Send failed to send iperf request to sendor %d - errno %d", id, send_result);
            return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 1);
        }
        
    }else{
        server_log("Error", "RCP no reference to socket or destination IP for id %d", id);
        return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 1);
    }
}

static xmlrpc_value * ping_request( xmlrpc_env *    const envP,
                                    xmlrpc_value *  const paramArrayP,
                                    void *          const serverInfo,
                                    void *          const channelInfo){

    xmlrpc_int32 id, dst_id, m_id, iterations;

    //parse argument array
    xmlrpc_decompose_value(envP, paramArrayP, "(iiii)", &id, &dst_id, &m_id, &iterations);
    if (envP->fault_occurred){
        server_log("Error", "Could not parse ping request argument array");
        return NULL;
    }

    //get socket
    struct nlist * sck = lookup((int) id);

    //get dst ip
    struct iplist * dst_ip = iplookup((int) dst_id);

    if(sck != NULL && dst_ip != NULL){
        //create request
        request_init((struct srrp_request *) send_buff, SRRP_RTT, (int) dst_id, dst_ip->ip, (int) m_id);       //ID & IP for server - temp
        add_param((struct srrp_request *) send_buff, SRRP_ITTR, (int) iterations);

        int send_result = send(sck->socket, send_buff, request_size((struct srrp_request *) send_buff), 0);

        //check if send was successful - socket has probably been closed
        if(send_result == request_size((struct srrp_request *) send_buff)){
            server_log("Info", "RPC sending ping request to sensor %d - bytes %d", id, send_result);
            return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 0);    
        }else{
            server_log("Error", "Send failed to send ping request to sendor %d - errno %d", id, send_result);
            return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 1);
        }
        
    }else{
        server_log("Error", "RCP no reference to socket or destination IP for id %d", id);
        return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 1);
    }
}

static xmlrpc_value * udp_request(  xmlrpc_env *    const envP,
                                    xmlrpc_value *  const paramArrayP,
                                    void *          const serverInfo,
                                    void *          const channelInfo){

    xmlrpc_int32 id, dst_id, m_id, speed, size, dur, dscp;

    //parse argument array
    xmlrpc_decompose_value(envP, paramArrayP, "(iiiiiii)", &id, &dst_id, &m_id, &speed, &size, &dur, &dscp);
    if (envP->fault_occurred){
        server_log("Error", "Could not parse udp request argument array");
        return NULL;
    }

    //get socket
    struct nlist * sck = lookup((int) id);

    //get dst ip
    struct iplist * dst_ip = iplookup((int) dst_id);

    if(sck != NULL && dst_ip !=NULL){

        //create request
        request_init((struct srrp_request *) send_buff, SRRP_UDP, dst_id,  dst_ip->ip, (int) m_id);  
        
        add_param((struct srrp_request *) send_buff, SRRP_SPEED, (int) speed);
        add_param((struct srrp_request *) send_buff, SRRP_SIZE, (int) size);
        add_param((struct srrp_request *) send_buff, SRRP_DUR, (int) dur);
        add_param((struct srrp_request *) send_buff, SRRP_DSCP, (int) dscp);

        //send
        int send_result = send(sck->socket, send_buff, request_size((struct srrp_request *) send_buff), 0);

        //check if send was successful - socket has probably been closed
        if(send_result == request_size((struct srrp_request *) send_buff)){
            server_log("Info", "RPC sending udp request to sensor %d", id);
            return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 0);    
        }else{
            server_log("Error", "Send failed to send udp request to sendor %d - errno %d", id, send_result);
            return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 1);
        }

    }else{
        server_log("Error", "RCP no reference to socket or destination IP for id %d", id);
        return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 1);
    }
}

static xmlrpc_value * dns_request(  xmlrpc_env *    const envP,
                                    xmlrpc_value *  const paramArrayP,
                                    void *          const serverInfo,
                                    void *          const channelInfo){

    int params = xmlrpc_array_size(envP, paramArrayP);
    server_log("Info", "DNS request %d params", params);

    xmlrpc_int32 id, m_id;
    const char * addr;
    const char * server;

    //parse argument array
    if(params == 1)
        xmlrpc_decompose_value(envP, paramArrayP, "(ii)", &id, &m_id);
    else if(params == 2)
        xmlrpc_decompose_value(envP, paramArrayP, "(iis)", &id, &m_id, &addr);
    else
        xmlrpc_decompose_value(envP, paramArrayP, "(iiss)", &id, &m_id, &addr, &server);

    if (envP->fault_occurred){
        server_log("Error", "Could not parse dns request argument array");
        return NULL;
    }
    
    //get socket
    struct nlist * sck = lookup((int) id);

    if(sck != NULL){
        //create request
        request_init((struct srrp_request *) send_buff, SRRP_DNS, 1, server_ip, (int) m_id);

        if(params > 1)
            add_param_string((struct srrp_request *) send_buff, SRRP_DN, (char *)addr);
        if(params > 2)
            add_param_string((struct srrp_request *) send_buff, SRRP_SERVER, (char *)server);

        int send_result = send(sck->socket, send_buff, request_size((struct srrp_request *) send_buff), 0);
        
        //check if send was successful - socket has probably been closed
        if(send_result == request_size((struct srrp_request *) send_buff)){
            server_log("Info", "RPC sending dns request to sensor %d - bytes %d", id, send_result);
            return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 0);    
        }else{
            server_log("Error", "Send failed to send dns request to sendor %d - errno %d", id, send_result);
            return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 1);
        }

    }else{
        server_log("Error", "RCP no reference to socket for id %d", id);
        return xmlrpc_build_value(envP, "i", (xmlrpc_int32) 1);
    }
}

/*
* Start RPC server
*/
void * rpc_server(void * arg){
    struct xmlrpc_method_info3 const ieprf_method = {
        /* .methodName     = */ "iperf.request",
        /* .methodFunction = */ &iperf_request,
    };
    struct xmlrpc_method_info3 const ping_method = {
        /* .methodName     = */ "ping.request",
        /* .methodFunction = */ &ping_request,
    };
    struct xmlrpc_method_info3 const udp_method = {
        /* .methodName     = */ "udp.request",
        /* .methodFunction = */ &udp_request,
    };
    struct xmlrpc_method_info3 const dns_method = {
        /* .methodName     = */ "dns.request",
        /* .methodFunction = */ &dns_request,
    };
    xmlrpc_server_abyss_parms serverparm;
    xmlrpc_registry * registryP;
    xmlrpc_env env;
    xmlrpc_server_abyss_t * serverP;
    xmlrpc_server_abyss_sig * oldHandlersP;

    xmlrpc_env_init(&env);

    xmlrpc_server_abyss_global_init(&env);

    registryP = xmlrpc_registry_new(&env);
    if (env.fault_occurred) {
        server_log("Error", "xmlrpc_registry_new() failed.  %s", env.fault_string);
        exit(1);
    }

    //add methods
    xmlrpc_registry_add_method3(&env, registryP, &ieprf_method);
    if (env.fault_occurred) {
        server_log("Error", "xmlrpc_registry_add_method3() iperf_method failed.  %s", env.fault_string);
        exit(1);
    }
    xmlrpc_registry_add_method3(&env, registryP, &ping_method);
    if (env.fault_occurred) {
        server_log("Error", "xmlrpc_registry_add_method3() ping_method failed.  %s", env.fault_string);
        exit(1);
    }
    xmlrpc_registry_add_method3(&env, registryP, &udp_method);
    if (env.fault_occurred) {
        server_log("Error", "xmlrpc_registry_add_method3() udp_method failed.  %s", env.fault_string);
        exit(1);
    }
    xmlrpc_registry_add_method3(&env, registryP, &dns_method);
    if (env.fault_occurred) {
        server_log("Error", "xmlrpc_registry_add_method3() dns_method failed.  %s", env.fault_string);
        exit(1);
    }

    serverparm.config_file_name = NULL;   /* Select the modern normal API */
    serverparm.registryP        = registryP;
    serverparm.port_number      = config.server_rpc_port;
    serverparm.runfirst         = NULL;
    serverparm.runfirst_arg     = NULL;
    serverparm.log_file_name    = "/var/log/weperf/rpc/server-rpc";

    xmlrpc_server_abyss_create(&env, &serverparm, XMLRPC_APSIZE(log_file_name), &serverP);
    //xmlrpc_server_abyss_setup_sig(&env, serverP, &oldHandlersP);

    //while(1){}

    server_log("Info", "Started XML-RPC server");

    xmlrpc_server_abyss_run_server(&env, serverP);

    if (env.fault_occurred) {
        printf("xmlrpc_server_abyss() failed.  %s\n", env.fault_string);
        exit(1);
    }

    //xmlrpc_server_abyss_restore_sig(oldHandlersP);
    //free(oldHandlersP);

    server_log("Info", "Stopping XML-RPC server");

    xmlrpc_server_abyss_destroy(serverP);

    xmlrpc_server_abyss_global_term();

    /*xmlrpc_server_abyss(&env, &serverparm, XMLRPC_APSIZE(log_file_name));
    if (env.fault_occurred) {
        printf("xmlrpc_server_abyss() failed.  %s\n", env.fault_string);
        exit(1);
    }*/
}

static void deamonise(){
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0){
        exit(EXIT_FAILURE);
    }

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    chdir("/");

    deamon = 1;

    server_log("Info", "Deamonised");
}

void stop_server(int sig){
    mysql_remove_all();
    server_log("Info", "Server stopped");
    exit(1);
}

char * iptos (uint32_t ipaddr) {
    static char ips[16];
   
    sprintf(ips, "%d.%d.%d.%d",
        (ipaddr >> 24),
        (ipaddr >> 16) & 0xff,
        (ipaddr >>  8) & 0xff,
        (ipaddr      ) & 0xff );
    return ips;
}

int mysql_connect(){
    
    conn = mysql_init(NULL);
    /* Connect to database */
    if (!mysql_real_connect(conn, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(conn));
        return 0;
    }
}

/*
* Returns the id
*/
int mysql_add_sensor(int id, char * ip, char * local_ip, char * ether){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char buff[500];

    char * query = "insert into sensors(sensor_id, ip, local_ip, ether, active, start%s) VALUES('%d', '%s', '%s', '%s', true, FROM_UNIXTIME(%d)%s) ON DUPLICATE KEY UPDATE ip=VALUES(ip), local_ip=VALUES(local_ip), ether=VALUES(ether), start=VALUES(start), active=true, sensor_id=LAST_INSERT_ID(sensor_id)\n";
    
    //add description if server sensor
    if(id == 1)
        sprintf(buff, query, ", description", id, ip, local_ip, ether, time(NULL), ", 'Server'");
    else
        sprintf(buff, query, "", id, ip, local_ip, ether, time(NULL), "");

    if (mysql_query(contd, buff)) {
      server_log("Error", "Database adding sensor - %s", mysql_error(contd));
      return -1;
    }

    int s_id = mysql_insert_id(contd);

    char * g_query = "insert into group_membership(group_id, sensor_id) VALUES(1, %d) ON DUPLICATE KEY UPDATE group_id=VALUES(group_id), sensor_id=VALUES(sensor_id)\n";
    sprintf(buff, g_query, s_id);

    if (mysql_query(contd, buff)){
        server_log("Error", "Database adding sensor to default group - %s", mysql_error(contd));
        return -1;
    }

    mysql_close(contd);

    return s_id;
}

int mysql_sensor_connected(int id){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char query[200];

    sprintf(query, "select active from sensors where sensor_id=%d", id);

    if(mysql_query(contd, query)){
        server_log("Error", "Databased checking connected sensor - %s", mysql_error(contd));
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(contd);
  
    if (result == NULL) {
        server_log("Error", "Database empty results checking connected sensor");
        return -1; 
    }

    MYSQL_ROW row = mysql_fetch_row(result);
        
    int active = atoi(row[0]);
    mysql_free_result(result);
    mysql_close(contd);

    return active;
}

int mysql_remove_sensor(int id){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char buff[200];
    char * query = "update sensors set active=false, end=FROM_UNIXTIME(%d) where sensor_id=%d";

    sprintf(buff, query, time(NULL), id);

    if (mysql_query(contd, buff)) {
      fprintf(stderr, "%s\n", mysql_error(contd));
      server_log("Error", "Database removing sensor - %s", mysql_error(contd));
      mysql_close(contd);
      return -1;
   }
   mysql_close(contd);
   return 1;
}

int mysql_remove_all(){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char * query = "update sensors set active=false\n";

    if (mysql_query(contd, query)) {
      fprintf(stderr, "%s\n", mysql_error(contd));
      server_log("Error", "Database removing sensor - %s", mysql_error(contd));
      mysql_close(contd);
      return -1;
   }
   mysql_close(contd);
   return 1;
}

int mysql_add_bw(int sensor_id, int dst_id, int m_id, float bandwidth, float duration, float bytes){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char buff[300];
    char * query = "insert into tcp(sensor_id, dst_id, measurement_id, bytes, duration, speed, time) values(%d, %d, %d, %f, %f, %f, FROM_UNIXTIME(%d))\n";

    sprintf(buff, query, sensor_id, dst_id, m_id, bytes, duration, bandwidth, time(NULL));

    if(mysql_query(contd, buff)){
        server_log("Error", "Database adding tcp - %s", mysql_error(contd));
        mysql_close(contd);
        return -1;
    }

    mysql_close(contd);
    return 1;
}

int mysql_add_rtt(int sensor_id, int dst_id, int m_id, float min, float max, float avg, float dev){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char buff[200];
    char * query = "insert into rtt(sensor_id, dst_id, measurement_id, min, max, avg, dev, time) values(%d, %d, %d, %f, %f, %f, %f, FROM_UNIXTIME(%d))\n";

    sprintf(buff, query, sensor_id, dst_id, m_id, min, max, avg, dev, time(NULL));

    if(mysql_query(contd, buff)){
        server_log("Error", "Database adding rtt - %s", mysql_error(contd));
        mysql_close(contd);
        return -1;
    }

    mysql_close(contd);
    return 1;
}

int mysql_add_udp(int sensor_id, int dst_id, int m_id, float size, float dur, float bw, float jit, float pkls, int dscp, float speed){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char buff[200];
    char * query = "insert into udp(sensor_id, dst_id, measurement_id, size, duration, bw, jitter, packet_loss, dscp_flag , send_bw, time) values(%d, %d, %d, %f, %f, %f, %f, %f, %d, %f, FROM_UNIXTIME(%d))\n";

    sprintf(buff, query, sensor_id, dst_id, m_id, size, dur, bw, jit, pkls, dscp, speed, time(NULL));

    if(mysql_query(contd, buff)){
        server_log("Error", "Database adding udp - %s", mysql_error(contd));
        mysql_close(contd);
        return -1;
    }

    mysql_close(contd);
    return 1;
}

int mysql_add_dns(int sensor_id, int m_id, float duration){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char buff[200];
    char * query = "insert into dns(sensor_id, measurement_id, duration,time) values(%d, %d, %f, FROM_UNIXTIME(%d))\n";

    sprintf(buff, query, sensor_id, m_id, duration, time(NULL));

    if(mysql_query(contd, buff)){
        server_log("Error", "Database adding dns - %s", mysql_error(contd));
        mysql_close(contd);
        return -1;
    }

    mysql_close(contd);
    return 1;
}

int mysql_add_dns_failure(int sensor_id, int m_id){
    MYSQL * contd;
    contd = mysql_init(NULL);

    if (!mysql_real_connect(contd, config.mysql_addr,
        config.mysql_usr, config.mysql_pass, "tnp", 0, NULL, 0)) {
        server_log("Error", "Database Connection - %s", mysql_error(contd));
        return 0;
    }

    char buff[200];
    char * query = "insert into dns_failure(sensor_id, measurement_id, time) values(%d, %d, FROM_UNIXTIME(%d))\n";

    sprintf(buff, query, sensor_id, time(NULL));

    if(mysql_query(contd, buff)){
        server_log("Error", "Database adding dns failure - %s", mysql_error(contd));
        mysql_close(contd);
        return -1;
    }

    mysql_close(contd);
    return 1;
}

int main(int argc, char ** argv) {

    //signal handler to close log file
    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);

    //casue zombies to be reaped automatically 
    if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
        perror(0);
        exit(1);
    }

    server_log("Info" , "Server Started");

    //parse configurations from file
    if (ini_parse("/etc/weperf/server-config.ini", handler, &config) < 0) {
        server_log("Error", "Can't load 'config.ini'\n");
        return 1;
    }

    //make deamon
    if(argc == 2)
        deamonise();

    //connect to database
    mysql_connect();

    int welcomeSocket;//, newSocket;
    char buffer[1024];
    struct sockaddr_in serverAddr, clientAddr;
    struct sockaddr_storage serverStorage;
    socklen_t addr_size;
    struct in_addr local_ip;        //storage for loacl ip

    addr_size = sizeof serverStorage;

    /*---- Create the socket. The three arguments are: ----*/
    /* 1) Internet domain 2) Stream socket 3) Default protocol (TCP in this case) */
    welcomeSocket = socket(PF_INET, SOCK_STREAM, 0);

    /*---- Configure settings of the server address struct ----*/
    /* Address family = Internet */
    serverAddr.sin_family = AF_INET;
    /* Set port number, using htons function to use proper byte order */
    serverAddr.sin_port = htons(config.server_port);
    /* Set IP address to localhost */
    serverAddr.sin_addr.s_addr = inet_addr(config.server_addr);
    /* Set all bits of the padding field to 0 */
    memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  

    /*---- Bind the address struct to the socket ----*/
    bind(welcomeSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));

    /*---- Listen on the socket, with 10 max connection requests queued ----*/
    if(listen(welcomeSocket,10)==0){
        server_log("Info" , "Listening on %s", config.server_addr);
    }else{
        server_log("Error" , "Failed to listen on %s", config.server_addr);
        exit(1);
    }

    //make server in_addr and remember 
    inet_aton(config.server_pub_addr, &server_ip);
    //ipinstall(1, server_ip);

    //start rpc server in thread
    pthread_t pth;
    pthread_create(&pth, NULL, rpc_server, (void * ) 1);

    //listen loop
    while(1){
        /*---- Accept call creates a new socket for the incoming connection ----*/
        int newSocket = accept(welcomeSocket, (struct sockaddr *) &clientAddr, &addr_size);

        server_log("Info", "Sensor connected, sending local_info request");

        //send request for MAC address
        request_init((struct srrp_request *) send_buff, SRRP_ETHER, 1, server_ip, 0);     //ID & IPfor server - temp
        send(newSocket, send_buff, request_size((struct srrp_request *) send_buff), 0);

        //wait for reply with MAC
        fd_set eth_fd;
        FD_ZERO(&eth_fd);
        FD_SET (newSocket, &eth_fd);

        //timeout for eth reply - 5 seconds
        struct timeval eth_tv;
        eth_tv.tv_sec = 5;
        eth_tv.tv_usec = 0;

        char * ether = malloc(18);  //MAC string

        if(select(newSocket + 1, &eth_fd, NULL, NULL, &eth_tv)){
            int bytes = recv(newSocket,recv_buff, sizeof(recv_buff),0);

            struct srrp_response * response;
            response = (struct srrp_response *) recv_buff;

            if(response->type == SRRP_ETHER){
                server_log("Info", "local_info reply");
                memcpy(ether, &response->results[0], 18);

                memcpy(&local_ip, &response->results[3], sizeof(local_ip));

                server_log("Info", "Loacal IP %s", inet_ntoa(local_ip));
            }else{
                server_log("Error", "Unknow response type %d - %d bytes", response->type, bytes);
                close(newSocket);
                continue;
            }
        }else{
            server_log("Error", "MAC address request timeout");
            close(newSocket);
            continue;
        }

        char addr[16];  //for IP address string
        inet_ntop(AF_INET, &clientAddr.sin_addr, addr, addr_size);

        //add to db
        int id;
        if(strcmp(ether, config.server_ether) ==  0){
            //server sensor
            id = mysql_add_sensor(1, addr, inet_ntoa(local_ip), ether);
        }else{
            //standalone sensor
            id = mysql_add_sensor(0, addr, inet_ntoa(local_ip), ether);
        }   

        server_log("Info" , "Sensor %s (%s) connected with id %d", ether, addr, id);

        //remember socket
        install(id, newSocket);

        //remember ip
        ipinstall(id, clientAddr.sin_addr);

        //receive loop - should start loops outside
        if(fork() == 0){

            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);

            //heartbeat loop
            int hb_pid;
            if((hb_pid = fork()) == 0){
                //build request
                request_init((struct srrp_request *) send_buff, SRRP_HB, 1, server_ip, 0);        //ID & IP for server - temp

                while(1){
                    if(!mysql_sensor_connected(id))
                        _exit(0);

                    printf("Sending hb request\n");
                    send(newSocket, send_buff, request_size((struct srrp_request *) send_buff), 0);
                    sleep(1);           //second
                }
            }

            //timeval for receive loop select()
            struct timeval tv;
            
            //watch newSocket for receive
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET (newSocket, &rfds);

            int bytes = 1;
            
            while(bytes){
                //reset timeout
                tv.tv_sec = config.server_timeout;
                tv.tv_usec = 0;
                
                int ready = select(newSocket + 1, &rfds, NULL, NULL, &tv);
                
                if(ready == -1){
                    server_log("Error", "select()");
                    perror("select()\n");
                }else if(ready){
                    bytes = recv(newSocket,recv_buff, sizeof(recv_buff),0);

                    struct srrp_response * response;
                    response = (struct srrp_response *) recv_buff;
                    
                    if(response->type == SRRP_HB){
                        printf("Received hb response\n");

                    }else if(response->type == SRRP_BW){
                        server_log("Info", "Recevied iperf response from sensor %d", id);

                        int i;
                        float bandwidth, duration, size = 0;
                        for(i = 0; i < response->length ; i++){
                            if(response->results[i].result == SRRP_RES_DUR){
                                memcpy(&duration, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_SIZE){
                                memcpy(&size, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_BW){
                                memcpy(&bandwidth, &response->results[i].value, 4);
                            }else{
                                server_log("Error", "Unrecoginsed result type for iperf test - %d", response->results[i].result);
                            }
                        }

                        //if all the results - add to db
                        if(bandwidth && duration && size){
                            mysql_add_bw(id, response->dst_id, response->m_id, bandwidth, duration, size);
                        }else{
                            server_log("Error", "Response missing iperf results from sensor %d", id);
                        }

                    }else if(response->type == SRRP_RTT){
                        server_log("Info", "Received ping response from sensor %d", id);

                        int i;
                        float avg, max, min, dev = 0;

                        for(i = 0; i < response->length ; i++){
                            if(response->results[i].result == SRRP_RES_RTTMAX){
                                //max = response->results[i].value;
                                memcpy(&max, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_RTTMIN){
                                //min = response->results[i].value;
                                memcpy(&min, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_RTTDEV){
                                //dev = response->results[i].value;
                                memcpy(&dev, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_RTTAVG){
                                //avg = response->results[i].value;
                                memcpy(&avg, &response->results[i].value, 4);
                            }else{
                                server_log("Error", "Unrecoginsed result type for ping test - %d", response->results[i].result);
                            }
                        }

                        if(avg && min && max){
                            mysql_add_rtt(id, response->dst_id, response->m_id, min, max, avg, dev);
                        }else{
                            server_log("Error", "Response missing ping results from sensor %d", id);
                        }

                    }else if(response->type == SRRP_UDP){
                        server_log("Info", "Received udp iperf response from sensor %d", id);

                        int i;
                        float dur, size, speed, dscp, bw, jit, pkls;

                        //parse results
                        for(i=0; i < response->length; i++){
                            if(response->results[i].result == SRRP_RES_DUR){
                                memcpy(&dur, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_SIZE){
                                memcpy(&size, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_BW){
                                memcpy(&bw, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_JTR){
                                memcpy(&jit, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_PKLS){
                                memcpy(&pkls, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_SPEED){
                                memcpy(&speed, &response->results[i].value, 4);
                            }else if(response->results[i].result == SRRP_RES_DSCP){
                                memcpy(&dscp, &response->results[i].value, 4);
                            }else{
                                server_log("Error", "Unrecognised result type for udp iperf");
                            }
                        }

                        //add to db
                        if(dur && size && bw && speed){
                            mysql_add_udp(id, response->dst_id, response->m_id, size, dur, bw, jit, pkls, (int) dscp, speed);
                        }else{
                            server_log("Error", "Response missing udp iperf results from sensor %d", id);
                        }

                    }else if(response->type == SRRP_DNS){
                        if(response->success == SRRP_SCES){
                            server_log("Info", "Received successfull dns response from sensor %d", id);

                            int i;
                            float dur;

                            for(i = 0; i < response->length; i++){
                                if(response->results[i].result == SRRP_RES_DUR){
                                    memcpy(&dur, &response->results[i].value, 4);
                                }
                            }

                            //check all results are there
                            if(dur)
                                mysql_add_dns(id, response->m_id, dur);
                            else
                                server_log("Error", "Response missing DNS results from sensor %d", id);

                        }else{
                            server_log("Info", "Received unsucessfull dns response from sensor %d", id);

                            mysql_add_dns_failure(id, response->m_id);
                        }

                    }else{
                        server_log("Error", "Uncognised data type - %d", response->type);
                    }

                }else{
                    server_log("Info", "Sensor %s timedout", inet_ntop(AF_INET, &clientAddr.sin_addr, addr, addr_size));
                    break;
                }
                    
            }
            
            server_log("Info", "Sensor %s discconected", inet_ntop(AF_INET, &clientAddr.sin_addr, addr, addr_size));
            
            //mark as disconnected in db 
            mysql_remove_sensor(id);

            //close comm socket
            close(newSocket);

            //kill hb loop (process)
            kill(hb_pid, SIGKILL);

            //kill comm (process)
            _exit(0);
        }
    }

    close(welcomeSocket);
    stop_server(0);

    return 0;
}
