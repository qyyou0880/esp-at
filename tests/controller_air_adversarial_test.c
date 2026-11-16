#include "air_config_parser.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int applies;
static int linked_gpio;
static char last_reply[128];
static bool apply(const air_uart_config_t *value,void *arg)
{ (void)value;(void)arg; ++applies;return true; }
static void response(const char *text,size_t len,void *arg)
{ (void)arg; assert(len<sizeof(last_reply));memcpy(last_reply,text,len);last_reply[len]=0; }
static bool link_apply(int gpio,bool save,void *arg)
{ (void)save;(void)arg;linked_gpio=gpio;return true; }

int main(void)
{
    air_config_session_t s;
    air_config_callbacks_t cb={.apply_uart=apply,.apply_link=link_apply,.reply=response};
    air_config_session_init(&s,NULL,1);
    s.authenticated=true;
    unsigned char overlong[180];
    memset(overlong,'x',97);
    const char *suffix="AT+BAUD=5\r\n";
    memcpy(overlong+97,suffix,strlen(suffix));
    air_config_feed(&s,overlong,97+strlen(suffix),1,&cb);
    if(applies){puts("FAIL: overlong command tail executed as a new command");return 1;}
    air_config_session_init(&s,NULL,2);
    s.authenticated=true;
    const unsigned char embedded_nul[]="AT+BAUD=5\0hidden\r\n";
    air_config_feed(&s,embedded_nul,sizeof(embedded_nul)-1,10,&cb);
    if(applies){puts("FAIL: embedded NUL accepted a truncated command");return 1;}
    air_config_session_init(&s,NULL,3);s.authenticated=true;
    const char *disable="AT+LINKGPIO=-1\r\n";
    air_config_feed(&s,(const uint8_t *)disable,strlen(disable),20,&cb);
    if(linked_gpio!=-1){puts("FAIL: disabling LINK did not pass -1 to the driver");return 1;}
    air_uart_config_t uncommon={230400,0,3};
    air_config_session_init(&s,&uncommon,4);s.authenticated=true;
    const char *baud="AT+BAUD?\r\n";
    air_config_feed(&s,(const uint8_t *)baud,strlen(baud),30,&cb);
    if(strstr(last_reply,"+OK=7")){puts("FAIL: unknown actual baud falsely reported as 115200");return 1;}
    air_config_session_init(&s,NULL,5);s.authenticated=true;
    const unsigned char bad_fragment[]={0,'x'};
    air_config_feed(&s,bad_fragment,sizeof(bad_fragment),100,&cb);
    air_config_flush_idle(&s,200,&cb);
    last_reply[0]=0;
    air_config_feed(&s,(const uint8_t *)"AT+BAUD?",8,210,&cb);
    air_config_flush_idle(&s,210+AIR_CONFIG_IDLE_DELIMITER_MS,&cb);
    if(strcmp(last_reply,"+OK=7\r\n")){puts("FAIL: invalid command without CRLF never recovers after idle");return 1;}
    puts("Adversarial framing checks passed");
    return 0;
}
