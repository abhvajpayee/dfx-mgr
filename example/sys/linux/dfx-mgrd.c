/*
 * Copyright (c) 2021, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <dfx-mgr/accel.h>
#include <dfx-mgr/shell.h>
#include <dfx-mgr/model.h>
#include <dfx-mgr/json-config.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

#define WATCH_PATH_LEN 256
#define MAX_WATCH 50

static int interrupted;
static acapd_accel_t *active_slots[3];
char *firmware_path = "/lib/firmware/xilinx";
char *config_path = "/etc/dfx-mgrd/daemon.conf";

sem_t mutex;

struct watch {
    int wd;
    char path[WATCH_PATH_LEN];
};

struct watch *active_watch = NULL;
struct basePLDesign *base_designs = NULL;
static int inotifyFd;

struct pss {
	struct lws_spa *spa;
};

/*static const char * const param_names[] = {
	"text1",
};

enum enum_param_names {
    EPN_TEXT1,
};*/
struct basePLDesign *findBaseDesign(const char *name){
	char *base_path = malloc(sizeof(char)*1024);
    int i;
	DIR *d;
	struct dirent *dir;

    d = opendir(firmware_path);
    if (d == NULL) {
		acapd_perror("Directory %s not found\n",firmware_path);
	}
	while((dir = readdir(d)) != NULL) {
		if (dir->d_type == DT_DIR) {
			if (strlen(dir->d_name) > 64) {
				continue;
			}
			if(strcmp(dir->d_name, name) == 0){
				sprintf(base_path,"%s/%s", firmware_path, dir->d_name);
				for (i = 0; i < MAX_WATCH; i++) {
					if (strcmp(base_designs[i].base_path, base_path) ==0) {
						acapd_debug("%s:found base %s\n",__func__,base_designs[i].base_path);
						return &base_designs[i];
					}
				}
			}
		}
	}
	return NULL;
}

char *get_accel_path(const char *name, int slot)
{
	char *accel_path = malloc(sizeof(char)*1024);
	DIR *d;
	struct dirent *dir;
	struct stat info;

    d = opendir(firmware_path);
    if (d == NULL) {
		acapd_perror("Directory %s not found\n",firmware_path);
	}
	while((dir = readdir(d)) != NULL) {
		if (dir->d_type == DT_DIR) {
			if (strlen(dir->d_name) > 64) {
				continue;
			}
			if(strcmp(dir->d_name, name) == 0){
				acapd_debug("Found dir %s in %s\n",name,firmware_path);
				sprintf(accel_path,"%s/%s/%s_slot%d", firmware_path, dir->d_name, dir->d_name,slot);
				if (stat(accel_path,&info) != 0)
					return NULL;
				if (info.st_mode & S_IFDIR){
					acapd_debug("Found accelerator path %s\n",accel_path);
					return accel_path;
				}
				else
					acapd_perror("No %s accel for slot %d found\n",name,slot);
			}
			else {
				acapd_debug("Found %s in %s\n",dir->d_name,firmware_path);
			}
		}
	}
	return NULL;
}
void getRMInfo()
{
	FILE *fptr;
	int i;
	char line[512];
	acapd_accel_t *accel;

	fptr = fopen("/home/root/rm_info.txt","w");
	if (fptr == NULL){
		acapd_perror("Couldn't create /home/root/rm_info.txt");
		goto out;
	}

	for (i = 0; i < 3; i++) {
		if(active_slots[i] == NULL)
			sprintf(line, "%d,%s",i,"GREY");
		else {
			accel = active_slots[i];
			sprintf(line,"%d,%s",i,accel->pkg->path);
		}
		fprintf(fptr,"\n%s",line);	
	}
out:
	fclose(fptr);
}

void update_env(char *path)
{
	DIR *FD;
	struct dirent *dir;
	int len, ret;
	char *str;
	char cmd[128];

	FD = opendir(path);
	if (FD) {
		while ((dir = readdir(FD)) != NULL) {
			len = strlen(dir->d_name);
            if (len > 7) {
                if (!strcmp(dir->d_name + (len - 7), ".xclbin") ||
						!strcmp(dir->d_name + (len - 7), ".XCLBIN")) {
                    str = (char *) calloc((len + strlen(path) + 1),
													sizeof(char));
                    sprintf(str, "%s/%s",path,dir->d_name);
					sprintf(cmd,"echo \"firmware: %s\" > /etc/vart.conf",str);
					ret = system(cmd);
					if (ret)
						acapd_perror("Write to /etc/vart.conf failed\n");
				}
			}
		}
	}
}

int load_accelerator(const char *accel_name)
{
	int i, ret;
	char *path;
	char shell_path[600];
	acapd_accel_t *accel = malloc(sizeof(acapd_accel_t));
	acapd_accel_pkg_hd_t *pkg = malloc(sizeof(acapd_accel_pkg_hd_t));
	struct basePLDesign *base = findBaseDesign(accel_name);

	if(base == NULL) {
		acapd_perror("No package found for %s\n",accel_name);
		return -1;
	} else
		sprintf(shell_path,"%s/shell.json",base->base_path);

	/* Flat shell designs which don't have any slot */
	if(base != NULL && strcmp(base->type,"XRT_FLAT") == 0) {
		if (active_slots[0] != NULL) {
			printf("Remove previously loaded accelerator, no empty slot\n");
			return -1;
		}
		sprintf(pkg->name,"%s",accel_name);
		acapd_debug("%s:loading xrt flat shell design %s\n",__func__,pkg->name);
		pkg->path = base->base_path;
		pkg->type = ACAPD_ACCEL_PKG_TYPE_NONE;
		init_accel(accel, pkg);
		accel->type = FLAT_SHELL;
		ret = load_accel(accel, shell_path, 0);
		if (ret < 0){
			acapd_perror("%s: Failed to load accel %s\n",__func__,accel_name);
			base->active = 0;
			return -1;
		}
		acapd_print("Loaded %s successfully\n",pkg->name);
		base->active = 1;
		active_slots[0] = accel;
		update_env(pkg->path);
		return 0;	
	}
	/* For SIHA slotted architecture */
	else if(base != NULL && strcmp(base->type,"SLOTTED") == 0) {
		for (i = 0; i < 3; i++) {
		if (active_slots[i] == NULL){
			path = get_accel_path(accel_name, i);
			if (path == NULL){
				acapd_debug("No accel package found for %s slot %d\n",accel_name,i);
				continue;
			}
			strcpy(pkg->name, accel_name);
			pkg->path = path;
			pkg->type = ACAPD_ACCEL_PKG_TYPE_NONE;
			init_accel(accel, pkg);
			acapd_debug("%s: Loading accel %s to slot %d \n", __func__, pkg->name,i);
			accel->rm_slot = i;
			/* Set rm_slot before load_accel() so isolation for appropriate slot can be applied*/

			ret = load_accel(accel, shell_path, 0);
			if (ret < 0){
				acapd_perror("%s: Failed to load accel %s\n",__func__,accel_name);
				return -1;
			}
			active_slots[i] = accel;
			getRMInfo();
			acapd_perror("%s:Loaded %s successfully\n",__func__,pkg->name);
			return accel->rm_slot;
		}
		}
		if (i >= 3)
			acapd_perror("Couldn't find empty slot for %s\n",accel_name);
	}
	else {
		acapd_perror("Not a valid accelerator package type.\n");
	}
	return -1;
}

void remove_accelerator(int slot)
{
	acapd_accel_t *accel = active_slots[slot];
	struct basePLDesign *base;
	if (active_slots == NULL || active_slots[slot] == NULL){
		acapd_perror("No Accel in slot %d\n",slot);
		return;
	}
	base = findBaseDesign(accel->pkg->name);
	printf("Removing accel %s \n",accel->pkg->path);
    remove_accel(accel, 0);
	free(accel);
	active_slots[slot] = NULL;
	base->active = 0;
	if (accel->type != FLAT_SHELL)
		getRMInfo();
}
void getFD(int slot)
{
	acapd_accel_t *accel = active_slots[slot];
	if (active_slots == NULL || active_slots[slot] == NULL){
		acapd_perror("%s No Accel in slot %d\n",__func__,slot);
		return;
	}
	get_fds(accel, slot);
}
void getShellFD(int slot)
{
	acapd_accel_t *accel = active_slots[slot];
	if (active_slots == NULL || active_slots[slot] == NULL){
		acapd_perror("%s No Accel in slot %d\n",__func__,slot);
		return;
	}
	get_shell_fd(accel);
}
void getClockFD(int slot)
{
	acapd_accel_t *accel = active_slots[slot];
	if (active_slots == NULL || active_slots[slot] == NULL){
		acapd_perror("%s No Accel in slot %d\n",__func__,slot);
		return;
	}
	get_shell_clock_fd(accel);
}
void listAccelerators(){
    int i;
	printf("%50s%15s%10s\n","Accelerator","Type","Active");
    for (i = 0; i < MAX_WATCH; i++) {
		if (base_designs[i].base_path[0] != '\0') {
			printf("%50s%15s%10d\n",&base_designs[i].base_path[strlen(firmware_path)+1],base_designs[i].type,
												base_designs[i].active);
		}
	}
}
struct msg{
    char cmd[32];
    char arg[128];
};
struct resp{
    char data[128];
    int len;
};
#define EXAMPLE_RX_BUFFER_BYTES sizeof(struct msg)
struct payload
{
    unsigned char data[LWS_SEND_BUFFER_PRE_PADDING + EXAMPLE_RX_BUFFER_BYTES + LWS_SEND_BUFFER_POST_PADDING];
    size_t len;
} received_payload;

unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + sizeof(struct resp) + LWS_SEND_BUFFER_POST_PADDING];

static int msgs;
static int callback_example( struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len )
{
    struct msg *m = (struct msg*)malloc(sizeof(struct msg));
    struct resp *r = (struct resp *)&buf[LWS_SEND_BUFFER_PRE_PADDING];
	int slot;

    switch( reason )
    {
		case LWS_CALLBACK_ESTABLISHED:
			lwsl_user("LWS_CALLBACK_ESTABLISHED user %s\n",(char *)user);
			break;

        case LWS_CALLBACK_RECEIVE:
            acapd_debug("LWS_CALLBACK_RECEIVE len%ld\n",len);
            memcpy( &received_payload.data[LWS_SEND_BUFFER_PRE_PADDING], in, len );
            received_payload.len = len;
            m = (struct msg *)&received_payload.data[LWS_SEND_BUFFER_PRE_PADDING];
            acapd_debug("server received cmd %s arg %s\n",m->cmd,m->arg);
			msgs++;
			if(strcmp(m->cmd,"-load") == 0){
				lwsl_user("Received %s \n",m->cmd);
				slot = load_accelerator(m->arg);
				sprintf(r->data,"%d",slot);
				r->len = 1;
				acapd_debug("daemon: load done slot %s len %d\n", r->data, r->len);
				lws_callback_on_writable_all_protocol( lws_get_context( wsi ), lws_get_protocol( wsi ) );
			}
			else if(strcmp(m->cmd,"-remove") == 0){
				lwsl_debug("Received %s slot %s\n",m->cmd,m->arg);
				r->len = 0;
				remove_accelerator(atoi(m->arg));
				lws_callback_on_writable_all_protocol( lws_get_context( wsi ), lws_get_protocol( wsi ) );
			}
			else if(strcmp(m->cmd,"-getFD") == 0){
				lwsl_debug("Received %s slot %s\n",m->cmd,m->arg);
				getFD(atoi(m->arg));
				sprintf(r->data,"%s","");
				r->len = 0;
				lws_callback_on_writable( wsi );
				lwsl_user("Server recieve done slot %s\n",m->arg);
			}
			else if(strcmp(m->cmd,"-getShellFD") == 0){
				lwsl_debug("Received %s slot %s\n",m->cmd,m->arg);
				sprintf(r->data,"%s","");
				r->len = 0;
				getShellFD(atoi(m->arg));
				lws_callback_on_writable_all_protocol( lws_get_context( wsi ), lws_get_protocol( wsi ) );
			}
			else if(strcmp(m->cmd,"-getClockFD") == 0){
				lwsl_debug("Received %s slot %s\n",m->cmd,m->arg);
				sprintf(r->data,"%s","");
				r->len = 0;
				getClockFD(atoi(m->arg));
				lws_callback_on_writable_all_protocol( lws_get_context( wsi ), lws_get_protocol( wsi ) );
			}
			else if(strcmp(m->cmd,"-getRMInfo") == 0){
				lwsl_debug("Received -getRMInfo\n");
				sprintf(r->data,"%s","");
				r->len = 0;
				getRMInfo();
				lws_callback_on_writable_all_protocol( lws_get_context( wsi ), lws_get_protocol( wsi ) );
			}
			else if(strcmp(m->cmd,"-listPackage") == 0){
				lwsl_debug("Received -listPackage\n");
				sprintf(r->data,"%s","");
				r->len = 0;
				listAccelerators();
				lws_callback_on_writable_all_protocol( lws_get_context( wsi ), lws_get_protocol( wsi ) );
			}
			else {
				lwsl_err("cmd not recognized\n");
				//return -1;
			}
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
			if (!msgs)
				return 0;
            lwsl_debug("LWS_CALLBACK_SERVER_WRITEABLE resp->len %d\n",r->len);
            lws_write( wsi, (unsigned char*)r, sizeof(struct resp), LWS_WRITE_TEXT );
			msgs--;
			//return -1;
            break;

        default:
            break;
    }

    return 0;
}

enum protocols
{
    PROTOCOL_HTTP = 0,
    PROTOCOL_EXAMPLE,
    PROTOCOL_COUNT
};


/*static char *requested_uri;
static const char *arg;
static int
callback_http(struct lws *wsi, enum lws_callback_reasons reason,
			void *user, void *in, size_t len)
{
	struct pss *pss = (struct pss *)user;
	int n;	
	switch (reason) {
	case LWS_CALLBACK_HTTP:
		acapd_debug("server LWS_CALLBACK_HTTP\n");
		requested_uri = (char *)in;
		break;

	case LWS_CALLBACK_HTTP_BODY:
		acapd_debug("server LWS_CALLBACK_HTTP_BODY\n");
		if(!pss->spa) {
			pss->spa = lws_spa_create(wsi, param_names, LWS_ARRAY_SIZE(param_names), 1024, NULL, NULL);
			if(!pss->spa)
				return -1;
		}
		if (lws_spa_process(pss->spa, in, (int)len))
			return -1;
		break;

	case LWS_CALLBACK_HTTP_BODY_COMPLETION:
		lwsl_user("server LWS_CALLBACK_HTTP_BODY_COMPLETION\n");
		lws_spa_finalize(pss->spa);
		if(pss->spa) {
			for(n = 0; n < (int)LWS_ARRAY_SIZE(param_names); n++) {
				if (!lws_spa_get_string(pss->spa, n))
					printf("undefined string in http body %s\n", param_names[n]);
				else {
					//printf("http body %s value %s\n",param_names[n],lws_spa_get_string(pss->spa, n));
					arg = lws_spa_get_string(pss->spa,n);
				}
			}
		}
		if(strcmp(requested_uri,"/loadpdi") == 0){
			lwsl_user("Received loadpdi \n");
			load_accelerator(arg,NULL);
		}
		else if(strcmp(requested_uri,"/remove") == 0){
			lwsl_user("Received remove \n");
			remove_accelerator(atoi(arg));
		}
		else if(strcmp(requested_uri,"/getFD") == 0){
			lwsl_user("Received getFD\n");
			getFD(atoi(arg));
		}
		else if(strcmp(requested_uri,"/getShellFD") == 0){
			lwsl_user("Received getShellFD\n");
			getShellFD(atoi(arg));
		}
		else if(strcmp(requested_uri,"/getClockFD") == 0){
			lwsl_user("Received getClockFD\n");
			getClockFD(atoi(arg));
		}
		else if(strcmp(requested_uri,"/getRMInfo") == 0){
			lwsl_user("Received getRMInfo\n");
			getRMInfo();
		}
		//if (pss->spa && lws_spa_destroy(pss->spa))
		//	return -1;
		if (lws_http_transaction_completed(wsi)) {
			return -1;
		}
	
		break;

	case LWS_CALLBACK_HTTP_DROP_PROTOCOL:
		printf("server LWS_CALLBACK_HTTP_DROP_PROTOCOL\n");
		if (pss->spa) {
			lws_spa_destroy(pss->spa);
			pss->spa = NULL;
		}
		break;
	
	default:
		break;
	}

	return 0;//lws_callback_http_dummy(wsi, reason, user, in, len);
}*/

static const struct lws_protocols protocols[] = { 
	// first protocol must always be HTTP handler
  {
    "http",			/* name */
    lws_callback_http_dummy,	/* callback */
    0,				/* per session data */
	0,				/* max frame size/ rx buffer */
	0, NULL, 0,
  },
  {
	"example-protocol",
	callback_example,
    0,
    EXAMPLE_RX_BUFFER_BYTES,
	0, NULL, 0,
  },
  { NULL, NULL, 0, 0, 0, NULL, 0 } /* terminator */
};

void sigint_handler(int sig)
{
	lwsl_err("server interrupted signal %d \n",sig);
	interrupted = 1;
}

//static void             /* Display information from inotify_event structure */
//displayInotifyEvent(struct inotify_event *i)
//{
    //printf("    wd =%2d; ", i->wd);
   // if (i->cookie > 0)
      //  printf("cookie =%4d; ", i->cookie);

//    printf("mask = ");
/*    if (i->mask & IN_ACCESS)        printf("IN_ACCESS ");
    if (i->mask & IN_ATTRIB)        printf("IN_ATTRIB ");
    if (i->mask & IN_CLOSE_NOWRITE) printf("IN_CLOSE_NOWRITE ");
    if (i->mask & IN_CLOSE_WRITE)   printf("IN_CLOSE_WRITE ");
    if (i->mask & IN_CREATE)        printf("IN_CREATE ");
    if (i->mask & IN_DELETE)        printf("IN_DELETE ");
    if (i->mask & IN_DELETE_SELF)   printf("IN_DELETE_SELF ");
    if (i->mask & IN_IGNORED)       printf("IN_IGNORED ");
    if (i->mask & IN_ISDIR)         printf("IN_ISDIR ");
    if (i->mask & IN_MODIFY)        printf("IN_MODIFY ");
    if (i->mask & IN_MOVE_SELF)     printf("IN_MOVE_SELF ");
    if (i->mask & IN_MOVED_FROM)    printf("IN_MOVED_FROM ");
    if (i->mask & IN_MOVED_TO)      printf("IN_MOVED_TO ");
    if (i->mask & IN_OPEN)          printf("IN_OPEN ");
    if (i->mask & IN_Q_OVERFLOW)    printf("IN_Q_OVERFLOW ");
    if (i->mask & IN_UNMOUNT)       printf("IN_UNMOUNT ");
    printf("\n");
	

   if (i->len > 0){}
        printf("        name = %s\n", i->name);
}*/

void add_to_watch(int wd, char *pathname)
{
    int i;
    for (i = 0; i < MAX_WATCH; i++) {
        if (active_watch[i].wd == -1) {
            //printf("adding watch to list %s\n",pathname);
            active_watch[i].wd = wd;
            strncpy(active_watch[i].path, pathname, WATCH_PATH_LEN -1);
            return;
        }
    }
    printf("no room to add more watch");
}

char * wd_to_pathname(int wd){
    int i;
    for (i = 0; i < MAX_WATCH; i++) {
        if (active_watch[i].wd == wd)
            return active_watch[i].path;
    }
    return NULL;
}

void remove_watch(char *path)
{
    int i;
    for (i = 0; i < MAX_WATCH; i++) {
        if (strcmp(active_watch[i].path, path) == 0){
        inotify_rm_watch(inotifyFd, active_watch[i].wd);
        active_watch[i].wd = -1;
        active_watch[i].path[0] = '\0';
        }
    }
}
void add_base_design(char *path){
    int i;
	char shell_path[600];
	struct stat info;

    for (i = 0; i < MAX_WATCH; i++) {
		if (base_designs[i].base_path[0] == '\0') {
			acapd_debug("adding base design %s\n",path);
			strncpy(base_designs[i].base_path, path, sizeof(base_designs[i].base_path)-1);
			base_designs[i].base_path[sizeof(base_designs[i].base_path) - 1] = '\0';
			sprintf(shell_path,"%s/shell.json",base_designs[i].base_path);

			/* If no shell.json provided, treat it as flat shell design */
			if (stat(shell_path,&info) != 0){
				base_designs[i].num_slots = 0;
				strncpy(base_designs[i].type,"XRT_FLAT", sizeof(base_designs[i].type)-1);
	            base_designs[i].type[sizeof(base_designs[i].type)-1] = '\0';
			}
			else {
				initBaseDesign(&base_designs[i], shell_path);
				base_designs[i].slots = (struct pl_slot*)malloc(sizeof(struct pl_slot) * base_designs[i].num_slots);
			}
			base_designs[i].active = 0;
			return;
		}
	}
}
void remove_base_design(char *path){
	acapd_debug("%s removing %s\n",__func__,path);
    int i;
    for (i = 0; i < MAX_WATCH; i++) {
		if (strcmp(base_designs[i].base_path, path) == 0) {
			base_designs[i].base_path[0] = '\0';
			base_designs[i].num_slots = 0;
			free(base_designs[i].slots);
			base_designs[i].active = 0;
			return;
		}
	}
}


#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))
void *threadFunc()
{
    int wd, j;
    char buf[BUF_LEN] __attribute__ ((aligned(8)));
    ssize_t numRead;
    char *p;
    char new_dir[512];
    struct inotify_event *event;
	DIR *d;
	struct dirent *dir;
	int new_dir_create = 0;
	
    active_watch = (struct watch *)malloc(MAX_WATCH * sizeof(struct watch));
    base_designs = (struct basePLDesign *)malloc(MAX_WATCH * sizeof(struct basePLDesign));

    for(j = 0; j < MAX_WATCH; j++) {
        active_watch[j].wd = -1;
        active_watch[j].path[0] = '\0';
    }

    for(j = 0; j < MAX_WATCH; j++) {
        base_designs[j].base_path[0] = '\0';
	}
    inotifyFd = inotify_init();                 /* Create inotify instance */
    if (inotifyFd == -1)
        acapd_perror("inotify_init failed\n");

    /* For each command-line argument, add a watch for all events */

    wd = inotify_add_watch(inotifyFd, firmware_path, IN_ALL_EVENTS);
    if (wd == -1) {
		acapd_perror("%s:%s not found, can't add inotify watch\n",__func__,firmware_path);
		exit(EXIT_SUCCESS);
	}
    add_to_watch(wd,firmware_path);

	/* Add already existing packages to inotify */
    d = opendir(firmware_path);
    if (d == NULL) {
		acapd_perror("Directory %s not found\n",firmware_path);
	}
	while((dir = readdir(d)) != NULL) {
		if (dir->d_type == DT_DIR) {
			if (strlen(dir->d_name) > 64 || strcmp(dir->d_name,".") == 0 ||
							strcmp(dir->d_name,"..") == 0) {
				continue;
			}
			sprintf(new_dir,"%s/%s", firmware_path, dir->d_name);
			acapd_debug("Found dir %s\n",new_dir);
			wd = inotify_add_watch(inotifyFd, new_dir, IN_ALL_EVENTS);
			if (wd == -1)
				acapd_perror("%s:inotify_add_watch failed on %s\n",__func__,new_dir);
			else {
				add_to_watch(wd, new_dir);
				add_base_design(new_dir);
			}
		}
	}
	/* Done parsing on target accelerators, now load a default one if present in config file */
	sem_post(&mutex);

	/* Listen for new updates in firmware path*/
    for (;;) {
        numRead = read(inotifyFd, buf, BUF_LEN);
        if (numRead == 0)
            acapd_perror("read() from inotify fd returned 0!");

        if (numRead == -1)
            acapd_perror("read() from inotify failed\n");

        /* Process all of the events in buffer returned by read() */

        for (p = buf; p < buf + numRead; ) {
            event = (struct inotify_event *) p;
            //displayInotifyEvent(event);
            if(event->mask & IN_CREATE || event->mask & IN_CLOSE_WRITE){
				if (event->mask & IN_ISDIR) {
					char * parent = wd_to_pathname(event->wd);
					sprintf(new_dir,"%s/%s",parent, event->name);
					acapd_debug("%s: add inotify watch on %s\n",__func__,new_dir);
					wd = inotify_add_watch(inotifyFd, new_dir, IN_ALL_EVENTS);
					if (wd == -1)
						printf("inotify_add_watch failed on %s\n",new_dir);
					else
						add_to_watch(wd, new_dir);
					new_dir_create = 1;
				}
            }
            else if((event->mask & IN_ATTRIB) && (event->mask & IN_ISDIR) &&
																new_dir_create) {
				add_base_design(new_dir);
				new_dir_create = 0;
            }
			else if((event->mask & IN_DELETE) && (event->mask & IN_ISDIR)) {
                char * parent = wd_to_pathname(event->wd);
                sprintf(new_dir,"%s/%s",parent, event->name);
				acapd_debug("%s:removing watch on  %s \n",__func__,new_dir);
                remove_watch(new_dir);
				remove_base_design(new_dir);
            }
            else if((event->mask & IN_MOVED_FROM) && (event->mask & IN_ISDIR)) {
                char * parent = wd_to_pathname(event->wd);
                sprintf(new_dir,"%s/%s",parent, event->name);
                remove_base_design(new_dir);
            }
            else if((event->mask & IN_MOVED_TO) && (event->mask & IN_ISDIR)) {
                char * parent = wd_to_pathname(event->wd);
                sprintf(new_dir,"%s/%s",parent, event->name);
				add_base_design(new_dir);
			}

            p += sizeof(struct inotify_event) + event->len;
        }
    }

    //exit(EXIT_SUCCESS);
}

int main(int argc, const char **argv)
{
	struct lws_context_creation_info info;
	struct lws_context *context;
	struct daemon_config config;
	pthread_t t;
	const char *p;

	int n = 0, logs = LLL_ERR | LLL_WARN
			/* for LLL_ verbosity above NOTICE to be built into lws,
			 * lws must have been configured and built with
			 * -DCMAKE_BUILD_TYPE=DEBUG instead of =RELEASE */
			/* | LLL_INFO */ /* | LLL_PARSER */ /* | LLL_HEADER */
			/* | LLL_EXT */ /* | LLL_CLIENT */ /* | LLL_LATENCY */
			/* | LLL_DEBUG */;

	acapd_debug("Starting http daemon\n");
	signal(SIGINT, sigint_handler);

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);
	lwsl_user("LWS minimal http server dynamic | visit http://localhost:7681\n");

	memset(&info, 0, sizeof info); /* otherwise uninitialized garbage */
	//info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
	//	       LWS_SERVER_OPTION_EXPLICIT_VHOSTS |
	//	LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

	info.port = 7681;
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
	info.vhost_name = "localhost";
	info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}


	if (!lws_create_vhost(context, &info)) {
		lwsl_err("Failed to create tls vhost\n");
		goto bail;
	}
	sem_init(&mutex, 0, 0);
	pthread_create(&t, NULL,threadFunc, NULL);
	sem_wait(&mutex);
	parse_config(config_path, &config);
	if (config.defaul_accel_name != NULL && strcmp(config.defaul_accel_name, "") != 0)
		load_accelerator(config.defaul_accel_name);
	while (n >= 0 && !interrupted)
		n = lws_service(context, 0);

bail:
	lws_context_destroy(context);

	return 0;
}