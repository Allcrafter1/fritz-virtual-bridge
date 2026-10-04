// SPDX-License-Identifier: MIT OR Apache-2.0
/* Supported only for the fingerprinted FRITZ!OS 8.25 ARM aha build. Preload
 * into aha, never other daemons. Reuses the genuine Nexus server transport.
 * Dynamic devices are managed through a mode-0600 Unix SEQPACKET control
 * socket. The fixed laboratory endpoints remain available only when the
 * explicit AHA_VIRTUAL_LEGACY=1 self-test mode is selected.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include "device_registry.h"

static ssize_t (*original_write)(int,const void*,size_t);
static ssize_t (*original_send)(int,const void*,size_t,int);
static int (*original_socketpair)(int,int,int,int[2]);
static int (*original_close)(int);
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int enabled, pairs[32][2], pair_count, server_fd=-1, incoming_fd=-1;
static unsigned generation, announced, switch_state, light_state, light_level=100;
static unsigned hanfun_light_state,hanfun_light_level=100;
static unsigned color_light_state,color_light_level=100,color_mode=2;
static unsigned color_temperature=2700,color_hue=120,color_saturation=255;
static unsigned color_full,color_is_unmapped;
/* FRITZ blind level: 0=open, 100=closed. MQTT exposes the inverse Home
 * Assistant cover position: 0=closed, 100=open. */
static unsigned cover_level=50,cover_openclose_state;
/* AVM HKR temperatures use half-degree values: 40 means 20.0 C. */
static unsigned thermostat_target=40,thermostat_reduced=36,thermostat_comfort=44;
static unsigned thermostat_actual=40,thermostat_offset,thermostat_battery=100;
static unsigned thermostat_heat_target=40;
static uint32_t thermostat_activated;
static unsigned thermostat_timer_mode=255;
static uint32_t thermostat_timer_end,thermostat_timer_activated;
/* Home Assistant owns the thermostat schedule.  FRITZ!OS receives a rolling
 * two-transition weekly shadow so the 440 can show the next real change. */
static unsigned thermostat_schedule_enabled;
static unsigned thermostat_schedule_current=40,thermostat_schedule_next=36;
static unsigned thermostat_schedule_current_minute,thermostat_schedule_next_minute;
static uint32_t thermostat_schedule_reject_until;
static unsigned command_count, last_command;
static int event_pipe[2]={-1,-1};
struct command_event {
    char endpoint[20];
    unsigned sequence,state,remote_id,level,has_level,has_color,has_cover,cover_action;
    unsigned has_thermostat,target_temperature;
    unsigned has_thermostat_timer,thermostat_timer_mode,thermostat_timer_previous;
    uint32_t thermostat_timer_end,thermostat_timer_duration;
    unsigned color_mode,color_temperature,hue,saturation;
};
static struct command_event event_queue[32];
static unsigned event_read, event_write;
static int debug_enabled,hanfun_experiment;
static uint32_t hanfun_timestamp;
/* A bounded per-device state adapter keeps the proven legacy encoders intact.
 * Selection and restoration occur only under lock; no selected context escapes
 * into another thread. Registry changes never alter an existing identity. */
static int legacy_enabled;
static fvb_device_registry registry;
static const fvb_device *selected_device;
struct device_values {
    uint32_t v_switch_state;
    uint32_t v_light_state;
    uint32_t v_light_level;
    uint32_t v_hanfun_light_state;
    uint32_t v_hanfun_light_level;
    uint32_t v_color_light_state;
    uint32_t v_color_light_level;
    uint32_t v_color_mode;
    uint32_t v_color_temperature;
    uint32_t v_color_hue;
    uint32_t v_color_saturation;
    uint32_t v_color_full;
    uint32_t v_color_is_unmapped;
    uint32_t v_cover_level;
    uint32_t v_cover_openclose_state;
    uint32_t v_thermostat_target;
    uint32_t v_thermostat_reduced;
    uint32_t v_thermostat_comfort;
    uint32_t v_thermostat_actual;
    uint32_t v_thermostat_offset;
    uint32_t v_thermostat_battery;
    uint32_t v_thermostat_heat_target;
    uint32_t v_thermostat_activated;
    uint32_t v_thermostat_timer_mode;
    uint32_t v_thermostat_timer_end;
    uint32_t v_thermostat_timer_activated;
    uint32_t v_thermostat_schedule_enabled;
    uint32_t v_thermostat_schedule_current;
    uint32_t v_thermostat_schedule_next;
    uint32_t v_thermostat_schedule_current_minute;
    uint32_t v_thermostat_schedule_next_minute;
    uint32_t v_thermostat_schedule_reject_until;
    uint32_t v_hanfun_timestamp;
};
static struct device_values device_values[FVB_REGISTRY_CAPACITY], saved_values, initial_values;
static unsigned unit_generation[FVB_REGISTRY_CAPACITY];
static void values_save(struct device_values *v){
    v->v_switch_state=switch_state;
    v->v_light_state=light_state;
    v->v_light_level=light_level;
    v->v_hanfun_light_state=hanfun_light_state;
    v->v_hanfun_light_level=hanfun_light_level;
    v->v_color_light_state=color_light_state;
    v->v_color_light_level=color_light_level;
    v->v_color_mode=color_mode;
    v->v_color_temperature=color_temperature;
    v->v_color_hue=color_hue;
    v->v_color_saturation=color_saturation;
    v->v_color_full=color_full;
    v->v_color_is_unmapped=color_is_unmapped;
    v->v_cover_level=cover_level;
    v->v_cover_openclose_state=cover_openclose_state;
    v->v_thermostat_target=thermostat_target;
    v->v_thermostat_reduced=thermostat_reduced;
    v->v_thermostat_comfort=thermostat_comfort;
    v->v_thermostat_actual=thermostat_actual;
    v->v_thermostat_offset=thermostat_offset;
    v->v_thermostat_battery=thermostat_battery;
    v->v_thermostat_heat_target=thermostat_heat_target;
    v->v_thermostat_activated=thermostat_activated;
    v->v_thermostat_timer_mode=thermostat_timer_mode;
    v->v_thermostat_timer_end=thermostat_timer_end;
    v->v_thermostat_timer_activated=thermostat_timer_activated;
    v->v_thermostat_schedule_enabled=thermostat_schedule_enabled;
    v->v_thermostat_schedule_current=thermostat_schedule_current;
    v->v_thermostat_schedule_next=thermostat_schedule_next;
    v->v_thermostat_schedule_current_minute=thermostat_schedule_current_minute;
    v->v_thermostat_schedule_next_minute=thermostat_schedule_next_minute;
    v->v_thermostat_schedule_reject_until=thermostat_schedule_reject_until;
    v->v_hanfun_timestamp=hanfun_timestamp;
}
static void values_load(const struct device_values *v){
    switch_state=v->v_switch_state;
    light_state=v->v_light_state;
    light_level=v->v_light_level;
    hanfun_light_state=v->v_hanfun_light_state;
    hanfun_light_level=v->v_hanfun_light_level;
    color_light_state=v->v_color_light_state;
    color_light_level=v->v_color_light_level;
    color_mode=v->v_color_mode;
    color_temperature=v->v_color_temperature;
    color_hue=v->v_color_hue;
    color_saturation=v->v_color_saturation;
    color_full=v->v_color_full;
    color_is_unmapped=v->v_color_is_unmapped;
    cover_level=v->v_cover_level;
    cover_openclose_state=v->v_cover_openclose_state;
    thermostat_target=v->v_thermostat_target;
    thermostat_reduced=v->v_thermostat_reduced;
    thermostat_comfort=v->v_thermostat_comfort;
    thermostat_actual=v->v_thermostat_actual;
    thermostat_offset=v->v_thermostat_offset;
    thermostat_battery=v->v_thermostat_battery;
    thermostat_heat_target=v->v_thermostat_heat_target;
    thermostat_activated=v->v_thermostat_activated;
    thermostat_timer_mode=v->v_thermostat_timer_mode;
    thermostat_timer_end=v->v_thermostat_timer_end;
    thermostat_timer_activated=v->v_thermostat_timer_activated;
    thermostat_schedule_enabled=v->v_thermostat_schedule_enabled;
    thermostat_schedule_current=v->v_thermostat_schedule_current;
    thermostat_schedule_next=v->v_thermostat_schedule_next;
    thermostat_schedule_current_minute=v->v_thermostat_schedule_current_minute;
    thermostat_schedule_next_minute=v->v_thermostat_schedule_next_minute;
    thermostat_schedule_reject_until=v->v_thermostat_schedule_reject_until;
    hanfun_timestamp=v->v_hanfun_timestamp;
}
static void select_device(const fvb_device *d){
    values_save(&saved_values);selected_device=d;
    values_load(&device_values[d-registry.devices]);
}
static void unselect_device(void){
    if(selected_device){values_save(&device_values[selected_device-registry.devices]);
        values_load(&saved_values);selected_device=NULL;}
}
static void provider_unlock(void){unselect_device();pthread_mutex_unlock(&lock);}
static unsigned profile_remote(fvb_device_profile p){
    static const unsigned ids[]={450,452,453,454,455};return ids[p];
}

#define DEBUG(...) do{if(debug_enabled)fprintf(stderr,__VA_ARGS__);}while(0)
static uint32_t handle;
static const char *control_path="/var/tmp/aha-virtual-provider.ctl";
static unsigned u16(const unsigned char*p){return ((unsigned)p[0]<<8)|p[1];}
static uint32_t u32(const unsigned char*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void p16(unsigned char*p,unsigned n){p[0]=n>>8;p[1]=n;}
static void p32(unsigned char*p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static void header(unsigned char*p,unsigned n,unsigned type){memset(p,0,n);p[0]=type;p[1]=3;p16(p+2,n);p32(p+4,handle);}
/* Encoder output uses the selected immutable identity. */
static ssize_t emit_write(int fd,const void *buf,size_t n){
    unsigned char packet[512];
    if(!selected_device)return original_write(fd,buf,n);
    if(n>sizeof(packet)||n<16)return -1;
    memcpy(packet,buf,n);p16(packet+8,selected_device->remote_id);
    if(packet[0]==4 && n==168){
        memset(packet+20,0,80);memcpy(packet+20,selected_device->name,strlen(selected_device->name));
        memset(packet+112,0,20);memcpy(packet+112,selected_device->uid,19);
    }else if(packet[0]==7 && n>=24){
        unsigned function=u32(packet+16);
        if(function==95 && n==48){memcpy(packet+24,selected_device->hanfun.ipui,5);
            p16(packet+32,selected_device->hanfun.discriminator);}
        if(function==98 && n==216){memset(packet+24,0,80);
            memcpy(packet+24,selected_device->name,strlen(selected_device->name));
            /* The local 8.25 ETSI receiver swaps this opaque interface array
             * again after decoding the network payload. */
            for(unsigned offset=112;offset<152;offset+=4){
                unsigned value=u32(packet+offset);
                packet[offset]=value;packet[offset+1]=value>>8;
                packet[offset+2]=value>>16;packet[offset+3]=value>>24;
            }
            p16(packet+152,selected_device->hanfun.unit_id);}
        if(function==118 && n>=36)p16(packet+24,selected_device->hanfun.unit_id);
    }
    return original_write(fd,packet,n);
}
/* lock held; packets are emitted with one write, no retry of partial packets. */
static const char *endpoint_for(unsigned remote_id){
    if(remote_id==455)return "VIRT000000000006";
    if(remote_id==454)return "VIRT000000000005";
    if(remote_id==453)return "VIRT000000000004";
    if(remote_id==452)return "VIRT000000000003";
    return remote_id==451?"VIRT000000000002":"VIRT000000000001";
}
static const char *cover_action_name(unsigned action){
    if(action==12)return "open";
    if(action==13)return "close";
    if(action==14)return "stop";
    return "set_position";
}
static int emit_relay(unsigned remote_id,unsigned value){
    unsigned char p[28];header(p,sizeof(p),7);p16(p+8,remote_id);
    p32(p+12,12);p32(p+16,15);p16(p+20,4);p32(p+24,value);
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_options(unsigned remote_id){
    unsigned char p[32];header(p,sizeof(p),7);p16(p+8,remote_id);
    p32(p+12,16);p32(p+16,35);p16(p+20,8);
    /* Options=0 and DeviceLock/SwitchLock=0. */
    p32(p+24,0);p32(p+28,0);
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_switch_config(void){
    unsigned char p[168];header(p,sizeof(p),4);p16(p+8,450);p[10]=2;
    p32(p+12,1);p32(p+16,0x200);
    strcpy((char*)p+20,"LAB Virtual HA Steckdose");
    p32(p+100,0xb74);p32(p+104,0x70001);
    strcpy((char*)p+112,"VIRT000000000001");strcpy((char*)p+132,"0.2-lab");
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_light_config(void){
    unsigned char p[168];header(p,sizeof(p),4);p16(p+8,451);p[10]=2;
    p32(p+12,1);
    /* SIMPLE_ONOFF | LIGHT | LEVEL_CONTROL listener flags. */
    p32(p+16,0x00580000);
    strcpy((char*)p+20,"LAB Virtual HA Licht");
    p32(p+100,0xb74);p32(p+104,0x000a0000);
    strcpy((char*)p+112,"VIRT000000000002");strcpy((char*)p+132,"0.1-lab");
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_light_modes_for(unsigned remote_id){
    unsigned char p[26];header(p,sizeof(p),7);p16(p+8,451);
    p16(p+8,remote_id);
    p32(p+12,10);p32(p+16,114);p16(p+20,2);
    /* DIM and SWITCHABLE. */
    p16(p+24,0x000c);
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_light_level_for(unsigned remote_id){
    unsigned char p[32];header(p,sizeof(p),7);p16(p+8,451);
    p16(p+8,remote_id);
    p32(p+12,16);p32(p+16,110);p16(p+20,8);
    /* Report an absolute percentage.  Raw-byte commands from FRITZ!OS are
     * normalized before this point, so the status describes the same value
     * without exposing the command representation. */
    p[24]=(unsigned char)light_level;p32(p+28,1); /* percentage */
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_light_modes(void){return emit_light_modes_for(451);}
static int emit_light_level(void){return emit_light_level_for(451);}
static int emit_hanfun_light_config(void){
    unsigned char p[168];header(p,sizeof(p),4);p16(p+8,452);p[10]=2;
    /* Network peers advertise HAN-FUN through the listener bit.  The device
     * type itself remains generic; type 14 is the local AHA representation
     * created after the ETSI configuration has been consumed. */
    p32(p+12,0);
    p32(p+16,0x00010000); /* HANFUN listener */
    strcpy((char*)p+20,"LAB Virtual HANFUN Licht");
    p32(p+100,0xb74);p32(p+104,0x000a0000);
    strcpy((char*)p+112,"VIRT000000000003");strcpy((char*)p+132,"0.1-lab");
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_color_light_config(void){
    unsigned char p[168];header(p,sizeof(p),4);p16(p+8,453);p[10]=2;
    p32(p+12,0);p32(p+16,0x00010000);
    strcpy((char*)p+20,"LAB Virtual HA Farbtemperatur");
    p32(p+100,0xb74);p32(p+104,0x000a0000);
    strcpy((char*)p+112,"VIRT000000000004");strcpy((char*)p+132,"0.1-lab");
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_cover_config(void){
    unsigned char p[168];header(p,sizeof(p),4);p16(p+8,454);p[10]=2;
    p32(p+12,0);p32(p+16,0x00010000);
    strcpy((char*)p+20,"LAB Virtual HA Rollladen");
    p32(p+100,0xb74);p32(p+104,0x000a0000);
    strcpy((char*)p+112,"VIRT000000000005");strcpy((char*)p+132,"0.1-lab");
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_thermostat_config(void){
    unsigned char p[168];header(p,sizeof(p),4);p16(p+8,455);p[10]=2;
    p32(p+12,1);             /* classic flat AVM/DECT device */
    p32(p+16,0x00000140);    /* external listener: HKR | temperature */
    strcpy((char*)p+20,"LAB Virtual HA Thermostat");
    p32(p+100,0xb74);p32(p+104,0x6000b); /* AVM / Thermo 302 profile */
    strcpy((char*)p+112,"VIRT000000000006");strcpy((char*)p+132,"0.1-lab");
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_direct_payload(unsigned remote_id,unsigned function,const void *body,unsigned body_length){
    unsigned char p[64];unsigned length=24+body_length;
    if(length>sizeof(p))return 0;
    header(p,length,7);p16(p+8,remote_id);
    p32(p+12,8+body_length);p32(p+16,function);p16(p+20,body_length);
    if(body_length)memcpy(p+24,body,body_length);
    return server_fd>=0 && emit_write(server_fd,p,length)==(ssize_t)length;
}
static int emit_thermostat_temperature(void){
    unsigned char body[8]={0};p32(body,thermostat_actual*5);p32(body+4,0);
    return emit_direct_payload(455,23,body,sizeof(body));
}
static int emit_thermostat_values(void){
    unsigned char body[16]={0};
    body[0]=(unsigned char)thermostat_target;
    body[1]=(unsigned char)thermostat_reduced;
    body[2]=(unsigned char)thermostat_comfort;
    body[3]=(unsigned char)thermostat_offset;
    body[4]=(unsigned char)thermostat_actual;
    p32(body+8,thermostat_activated);
    return emit_direct_payload(455,57,body,sizeof(body));
}
static int emit_thermostat_state(void){
    unsigned char body[4]={0,0,0,0};body[3]=(unsigned char)thermostat_battery;
    return emit_direct_payload(455,58,body,sizeof(body));
}
static int emit_thermostat_timer(void){
    unsigned char body[16]={0};
    p32(body,thermostat_timer_mode);p32(body+4,thermostat_timer_end);
    p32(body+8,thermostat_timer_activated);body[12]=255;
    /* These two constant bytes are also present in commands generated by
     * FRITZ!OS 8.25 for both Boost and window-open/cold overrides. */
    body[13]=0x20;body[14]=0x02;
    return emit_direct_payload(455,117,body,sizeof(body));
}
static int emit_thermostat_schedule(void){
    unsigned char body[16]={0};
    p32(body,1); /* weekly table */
    if(!thermostat_schedule_enabled)return emit_direct_payload(455,55,body,8);
    p16(body+4,1);p16(body+6,2);
    unsigned current_native=(thermostat_schedule_current_minute+1440)%10080;
    unsigned next_native=(thermostat_schedule_next_minute+1440)%10080;
    uint32_t current_entry=(current_native<<8)|(1u<<5)|(1u<<2);
    uint32_t next_entry=(next_native<<8)|(0u<<5)|(1u<<2);
    /* Function 55 is ordered in its Sunday-based native week.  The loop bit
     * is set on every entry except the last array element. */
    if(current_native<next_native){
        p32(body+8,current_entry|1u);p32(body+12,next_entry);
    }else{
        p32(body+8,next_entry|1u);p32(body+12,current_entry);
    }
    return emit_direct_payload(455,55,body,sizeof(body));
}
static int emit_thermostat_status(void){
    return emit_thermostat_temperature() && emit_thermostat_values() &&
           emit_thermostat_state() && emit_thermostat_timer() &&
           emit_thermostat_schedule();
}
static int emit_hanfun_device_config_for(unsigned remote_id,unsigned ipui_suffix,unsigned discriminator){
    unsigned char p[48];header(p,sizeof(p),7);p16(p+8,452);
    p16(p+8,remote_id);
    p32(p+12,32);p32(p+16,95);p16(p+20,24);
    /* Stable five-byte IPUI, no application-protocol extension. */
    p[24]=0x76;p[25]=0x69;p[26]=0x72;p[27]=0x74;p[28]=(unsigned char)ipui_suffix;
    p16(p+32,discriminator);
    p32(p+44,hanfun_timestamp); /* strictly newer device-config timestamp */
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_hanfun_device_config(void){return emit_hanfun_device_config_for(452,3,3);}
static int emit_color_device_config(void){return emit_hanfun_device_config_for(453,4,4);}
static int emit_cover_device_config(void){return emit_hanfun_device_config_for(454,5,5);}
static int emit_hanfun_unit_config_for(unsigned remote_id,const char *name,unsigned unit_type,int color){
    unsigned char p[216];header(p,sizeof(p),7);p16(p+8,452);
    p16(p+8,remote_id);
    p32(p+12,200);p32(p+16,98);p16(p+20,192);
    strncpy((char*)p+24,name,79);
    p32(p+104,0);       /* legacy sort index */
    p32(p+108,unit_type);
    p32(p+112,512);     /* HF_IF_ON_OFF */
    p32(p+116,513);     /* HF_IF_LEVEL_CTRL */
    if(color)p32(p+120,514); /* HF_IF_COLOR_CTRL */
    p16(p+152,1);       /* unit id */
    p32(p+160,hanfun_timestamp); /* strictly newer unit-config timestamp */
    /* Body+140 contains a mandatory 52-byte switch_action_config header.
     * It remains zeroed, including its action count at Body+166. */
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_hanfun_unit_config(void){
    return emit_hanfun_unit_config_for(452,"LAB Virtual HA Licht",265,0);
}
static int emit_color_unit_config(void){
    return emit_hanfun_unit_config_for(453,"LAB Virtual HA Farbtemperatur",278,1);
}
static int emit_cover_unit_config(void){
    unsigned char p[216];header(p,sizeof(p),7);p16(p+8,454);
    p32(p+12,200);p32(p+16,98);p16(p+20,192);
    strcpy((char*)p+24,"LAB Virtual HA Rollladen");
    p32(p+104,0);p32(p+108,281); /* HF_UNIT_TYPE_BLIND */
    p32(p+112,513);              /* HF_IF_LEVEL_CTRL */
    p32(p+116,516);              /* HF_IF_OPEN_CLOSE */
    p32(p+120,517);              /* HF_IF_OPEN_CLOSE_CONFIG */
    p16(p+152,1);p32(p+160,hanfun_timestamp);
    return server_fd>=0 && emit_write(server_fd,p,sizeof(p))==(ssize_t)sizeof(p);
}
static int emit_hanfun_unit_payload_for(unsigned remote_id,unsigned function,const void *body,unsigned body_length){
    unsigned char p[64];unsigned length=36+body_length;
    if(length>sizeof(p))return 0;
    header(p,length,7);p16(p+8,remote_id);
    p32(p+12,length-16);p32(p+16,118);p16(p+20,length-24);
    p16(p+24,1);p32(p+28,function);p16(p+32,body_length);
    if(body_length)memcpy(p+36,body,body_length);
    return server_fd>=0 && emit_write(server_fd,p,length)==(ssize_t)length;
}
static int emit_hanfun_unit_payload(unsigned function,const void *body,unsigned body_length){
    return emit_hanfun_unit_payload_for(452,function,body,body_length);
}
static int emit_hanfun_relay(void){
    unsigned char body[4];p32(body,hanfun_light_state);
    return emit_hanfun_unit_payload(15,body,sizeof(body));
}
static int emit_hanfun_level(void){
    unsigned char body[8]={0};body[0]=(unsigned char)hanfun_light_level;p32(body+4,1);
    return emit_hanfun_unit_payload(110,body,sizeof(body));
}
static int emit_color_capabilities(void){
    unsigned char body[4]={0};p16(body,color_full?0x0005:0x0004);
    body[2]=(unsigned char)color_full;
    return emit_hanfun_unit_payload_for(453,109,body,sizeof(body));
}
static int emit_color_relay(void){
    unsigned char body[4];p32(body,color_light_state);
    return emit_hanfun_unit_payload_for(453,15,body,sizeof(body));
}
static int emit_color_level(void){
    unsigned char body[8]={0};body[0]=(unsigned char)color_light_level;p32(body+4,1);
    return emit_hanfun_unit_payload_for(453,110,body,sizeof(body));
}
static int emit_color_value(void){
    unsigned char body[16]={0};
    p32(body,color_mode);
    if(color_mode==2){p16(body+4,color_temperature);return emit_hanfun_unit_payload_for(453,108,body,10);}
    p16(body+4,color_hue);body[6]=(unsigned char)color_saturation;
    body[8]=(unsigned char)color_is_unmapped;
    return emit_hanfun_unit_payload_for(453,108,body,16);
}
static int emit_color_status(void){
    return emit_color_capabilities() && emit_color_relay() &&
           emit_color_level() && emit_color_value();
}
static int emit_cover_level(void){
    unsigned char body[8]={0};body[0]=(unsigned char)cover_level;p32(body+4,1);
    return emit_hanfun_unit_payload_for(454,110,body,sizeof(body));
}
static int emit_cover_openclose_status(void){
    unsigned char body[6]={0};p32(body,15);p16(body+4,cover_openclose_state);
    return emit_hanfun_unit_payload_for(454,125,body,sizeof(body));
}
static int emit_cover_status(void){
    return emit_cover_level() && emit_cover_openclose_status();
}
static int emit_legacy_all(void){
    int ok=emit_switch_config() && emit_options(450) && emit_relay(450,switch_state) &&
           emit_light_config() && emit_options(451) && emit_light_modes() &&
           emit_relay(451,light_state) && emit_light_level();
    /* Unit creation and initial status are deliberately separate control
     * operations.  This lets the operator verify that Function95 was
     * accepted before Function98, and that the Unit exists before sending
     * Function118 status traffic. */
    if(ok&&hanfun_experiment)
        ok=emit_hanfun_light_config() && emit_hanfun_device_config() &&
           emit_color_light_config() && emit_color_device_config() &&
           emit_cover_config() && emit_cover_device_config() &&
           emit_thermostat_config() && emit_thermostat_status();
    return ok;
}
/* These functions are called with lock held. UNIT remains an explicit second
 * provisioning step: enqueueing Function95 does not prove parent acceptance. */
static int dynamic_status(const fvb_device *d){
    switch(d->profile){
    case FVB_PROFILE_SWITCH:return emit_relay(450,switch_state);
    case FVB_PROFILE_DIMMABLE_LIGHT:return emit_hanfun_relay()&&emit_hanfun_level();
    case FVB_PROFILE_COLOR_TEMPERATURE_LIGHT:return emit_color_status();
    case FVB_PROFILE_COVER:return emit_cover_status();
    case FVB_PROFILE_THERMOSTAT:return emit_thermostat_status();
    default:return 0;
    }
}
static int dynamic_announce(const fvb_device *d){
    uint32_t now=(uint32_t)time(NULL);
    if(hanfun_timestamp==UINT32_MAX)return 0;
    hanfun_timestamp=now>hanfun_timestamp?now:hanfun_timestamp+1;
    unit_generation[d-registry.devices]=0;
    switch(d->profile){
    case FVB_PROFILE_SWITCH:return emit_switch_config()&&emit_options(450)&&dynamic_status(d);
    case FVB_PROFILE_DIMMABLE_LIGHT:return emit_hanfun_light_config()&&emit_hanfun_device_config();
    case FVB_PROFILE_COLOR_TEMPERATURE_LIGHT:return emit_color_light_config()&&emit_color_device_config();
    case FVB_PROFILE_COVER:return emit_cover_config()&&emit_cover_device_config();
    case FVB_PROFILE_THERMOSTAT:return emit_thermostat_config()&&dynamic_status(d);
    default:return 0;
    }
}
static int dynamic_unit(const fvb_device *d){
    int ok=0;
    if(server_fd<0)return 0;
    if(hanfun_timestamp==UINT32_MAX)return 0;
    ++hanfun_timestamp;
    switch(d->profile){
    case FVB_PROFILE_DIMMABLE_LIGHT:ok=emit_hanfun_unit_config();break;
    case FVB_PROFILE_COLOR_TEMPERATURE_LIGHT:ok=emit_color_unit_config();break;
    case FVB_PROFILE_COVER:ok=emit_cover_unit_config();break;
    default:return 0;
    }
    if(ok){unit_generation[d-registry.devices]=generation;ok=dynamic_status(d);}
    return ok;
}
static int emit_all(void){
    int ok=legacy_enabled?emit_legacy_all():1;
    for(size_t i=0;i<registry.count;++i){
        const fvb_device *d=fvb_registry_at(&registry,i);
        if(d->enabled){select_device(d);int result=dynamic_announce(d);unselect_device();if(!result)ok=0;}
    }
    return ok;
}
/* >=0 handled; -1 dispatch compatible feedback through the existing parser.
 * cmd is rewritten only after profile and enabled-state validation. */
static int dynamic_control(char *cmd,size_t capacity){
    char operation[24],uid[20],rest[160];int offset=0;
    if(sscanf(cmd,"%23s %19s %n",operation,uid,&offset)<2)return -1;
    if(!strcmp(operation,"ADD")||!strcmp(operation,"RESTORE")){
        unsigned restored_id=0;
        if(!strcmp(operation,"RESTORE")){
            char *end=NULL;
            unsigned long value=strtoul(cmd+offset,&end,10);
            if(end==cmd+offset||*end!=' '||value<FVB_FIRST_REMOTE_ID||value>UINT16_MAX)return 0;
            restored_id=(unsigned)value;offset=(int)(end-cmd)+1;
        }
        char profile[40];int name_offset=0;
        if(sscanf(cmd+offset,"%39s %n",profile,&name_offset)!=1||!name_offset)return 0;
        fvb_device_profile p;
        for(p=0;p<FVB_PROFILE_COUNT;++p)if(!strcmp(profile,fvb_profile_name(p)))break;
        const fvb_device *existing=fvb_registry_find(&registry,uid);
        if(existing)return (!restored_id||existing->remote_id==restored_id)&&existing->profile==p&&!strcmp(existing->name,cmd+offset+name_offset);
        uint16_t id;
        uint32_t watermark=registry.next_remote_id;
        if(restored_id){
            if(fvb_registry_find_remote(&registry,(uint16_t)restored_id))return 0;
            registry.next_remote_id=restored_id;
        }
        fvb_registry_result added=fvb_registry_add(&registry,registry.revision,uid,cmd+offset+name_offset,p,&id);
        if(added!=FVB_REGISTRY_OK){registry.next_remote_id=watermark;return 0;}
        if(registry.next_remote_id<watermark)registry.next_remote_id=watermark;
        const fvb_device *d=fvb_registry_find_remote(&registry,id);
        device_values[d-registry.devices]=initial_values;
        /* Configuration is accepted offline; ANNOUNCE/transport reconnect retries it. */
        if(server_fd>=0){select_device(d);(void)dynamic_announce(d);unselect_device();}
        return 1;
    }
    int management=!strcmp(operation,"RENAME")||!strcmp(operation,"ENABLE")||
        !strcmp(operation,"DISABLE")||!strcmp(operation,"REMOVE")||
        !strcmp(operation,"UNIT")||!strcmp(operation,"ANNOUNCE");
    if(!fvb_registry_valid_uid(uid))return management?0:-1;
    const fvb_device *d=fvb_registry_find(&registry,uid);
    /* REMOVE is deliberately idempotent so an interrupted prune can be retried. */
    if(!d)return !strcmp(operation,"REMOVE");
    if(!strcmp(operation,"RENAME")){
        if(fvb_registry_rename(&registry,registry.revision,uid,cmd+offset)!=FVB_REGISTRY_OK)return 0;
        if(d->enabled&&server_fd>=0){select_device(d);(void)dynamic_announce(d);unselect_device();}
        return 1;
    }
    if(!strcmp(operation,"ENABLE")||!strcmp(operation,"DISABLE")){
        if(cmd[offset])return 0;
        int on=!strcmp(operation,"ENABLE");
        if(fvb_registry_set_enabled(&registry,registry.revision,uid,on)!=FVB_REGISTRY_OK)return 0;
        unit_generation[d-registry.devices]=0;
        if(on&&server_fd>=0){select_device(d);(void)dynamic_announce(d);unselect_device();}
        /* Disable is administrative: no native removal packet is invented. */
        return 1;
    }
    if(!strcmp(operation,"REMOVE")){
        if(cmd[offset])return 0;
        size_t index=(size_t)(d-registry.devices);
        if(fvb_registry_remove(&registry,registry.revision,uid)!=FVB_REGISTRY_OK)return 0;
        if(index<registry.count){
            memmove(&device_values[index],&device_values[index+1],
                    (registry.count-index)*sizeof(device_values[0]));
            memmove(&unit_generation[index],&unit_generation[index+1],
                    (registry.count-index)*sizeof(unit_generation[0]));
        }
        memset(&device_values[registry.count],0,sizeof(device_values[0]));
        unit_generation[registry.count]=0;
        return 1;
    }
    if(!d->enabled)return 0;
    if(!strcmp(operation,"UNIT")||!strcmp(operation,"ANNOUNCE")){
        if(cmd[offset])return 0;
        select_device(d);int ok=!strcmp(operation,"UNIT")?dynamic_unit(d):dynamic_announce(d);
        unselect_device();return ok;
    }
    int light=d->profile==FVB_PROFILE_DIMMABLE_LIGHT||d->profile==FVB_PROFILE_COLOR_TEMPERATURE_LIGHT;
    int allowed=(!strcmp(operation,"SET")&&(light||d->profile==FVB_PROFILE_SWITCH))||
        (!strcmp(operation,"LEVEL")&&light)||
        (!strcmp(operation,"COLOR_TEMP")&&d->profile==FVB_PROFILE_COLOR_TEMPERATURE_LIGHT)||
        (!strcmp(operation,"POSITION")&&d->profile==FVB_PROFILE_COVER)||
        (d->profile==FVB_PROFILE_THERMOSTAT&&(!strcmp(operation,"TARGET")||!strcmp(operation,"MODE")||
            !strcmp(operation,"TIMER")||!strcmp(operation,"SCHEDULE")));
    if(!allowed||strlen(cmd+offset)>=sizeof(rest))return 0;
    if(d->hanfun.unit_id&&unit_generation[d-registry.devices]!=generation)return 0;
    strcpy(rest,cmd+offset);select_device(d);
    int n=snprintf(cmd,capacity,"%s %s %s",operation,endpoint_for(profile_remote(d->profile)),rest);
    if(n<0||(size_t)n>=capacity){unselect_device();return 0;}
    return -1;
}

/* lock held; queue every FRITZ!-originated command for the bridge worker. */
static void queue_command_event(unsigned remote_id,unsigned has_level,unsigned has_color,
                                unsigned has_cover,unsigned cover_action){
    unsigned next=(event_write+1)%32;
    if(next==event_read)event_read=(event_read+1)%32;
    memset(&event_queue[event_write],0,sizeof(event_queue[event_write]));
    strcpy(event_queue[event_write].endpoint,selected_device?selected_device->uid:endpoint_for(remote_id));
    event_queue[event_write].sequence=command_count;
    event_queue[event_write].state=remote_id==454?(cover_level<100):
                                    (remote_id==453?color_light_state:
                                    (remote_id==452?hanfun_light_state:
                                    (remote_id==451?light_state:switch_state)));
    event_queue[event_write].remote_id=remote_id;
    event_queue[event_write].level=remote_id==454?(100-cover_level):
                                    (remote_id==453?color_light_level:
                                    (remote_id==452?hanfun_light_level:light_level));
    event_queue[event_write].has_level=has_level;
    event_queue[event_write].has_color=has_color;
    event_queue[event_write].has_cover=has_cover;
    event_queue[event_write].cover_action=cover_action;
    event_queue[event_write].color_mode=color_mode;
    event_queue[event_write].color_temperature=color_temperature;
    event_queue[event_write].hue=color_hue;
    event_queue[event_write].saturation=color_saturation;
    event_write=next;
    if(event_pipe[1]>=0){
        unsigned char wake=1;
        (void)syscall(SYS_write,event_pipe[1],&wake,1);
    }
}
static void queue_cover_event(unsigned action){queue_command_event(454,0,0,1,action);}
static void queue_thermostat_event(void){
    unsigned next=(event_write+1)%32;
    if(next==event_read)event_read=(event_read+1)%32;
    memset(&event_queue[event_write],0,sizeof(event_queue[event_write]));
    strcpy(event_queue[event_write].endpoint,selected_device?selected_device->uid:endpoint_for(455));
    event_queue[event_write].sequence=command_count;
    event_queue[event_write].remote_id=455;
    event_queue[event_write].has_thermostat=1;
    event_queue[event_write].target_temperature=thermostat_target;
    event_write=next;
    if(event_pipe[1]>=0){unsigned char wake=1;(void)syscall(SYS_write,event_pipe[1],&wake,1);}
}
static void queue_thermostat_timer_event(unsigned mode,unsigned previous,uint32_t end,uint32_t duration){
    unsigned next=(event_write+1)%32;
    if(next==event_read)event_read=(event_read+1)%32;
    memset(&event_queue[event_write],0,sizeof(event_queue[event_write]));
    strcpy(event_queue[event_write].endpoint,selected_device?selected_device->uid:endpoint_for(455));
    event_queue[event_write].sequence=command_count;
    event_queue[event_write].remote_id=455;
    event_queue[event_write].has_thermostat_timer=1;
    event_queue[event_write].thermostat_timer_mode=mode;
    event_queue[event_write].thermostat_timer_previous=previous;
    event_queue[event_write].thermostat_timer_end=end;
    event_queue[event_write].thermostat_timer_duration=duration;
    event_write=next;
    if(event_pipe[1]>=0){unsigned char wake=1;(void)syscall(SYS_write,event_pipe[1],&wake,1);}
}
int socketpair(int domain,int type,int protocol,int sv[2]){
    if(!original_socketpair)original_socketpair=dlsym(RTLD_NEXT,"socketpair");
    int r=original_socketpair(domain,type,protocol,sv);
    if(enabled && !r && domain==AF_UNIX && type==SOCK_STREAM){
        pthread_mutex_lock(&lock);
        if(pair_count<32){pairs[pair_count][0]=sv[0];pairs[pair_count++][1]=sv[1];}
        provider_unlock();
    }return r;
}
ssize_t write(int fd,const void*buf,size_t n){
    if(!original_write)return syscall(SYS_write,fd,buf,n);
    const unsigned char*p=buf;
    if(enabled && n>=16 && p[0]==1 && p[1]==3 && u16(p+2)==n){
        char name[16]={0};prctl(PR_GET_NAME,name,0,0,0);
        if(!strncmp(name,"sR/TX",5)){
            pthread_mutex_lock(&lock);
            for(int i=0;i<pair_count;i++){
                int peer=pairs[i][0]==fd?pairs[i][1]:(pairs[i][1]==fd?pairs[i][0]:-1);
                if(peer>=0){server_fd=fd;incoming_fd=peer;handle=u32(p+8);generation++;break;}
            }
            provider_unlock();
        }
    }
    return original_write(fd,buf,n);
}
ssize_t send(int fd,const void*buf,size_t n,int flags){
    if(!original_send)original_send=dlsym(RTLD_NEXT,"send");
    const unsigned char*p=buf;
    if(enabled && n>=24 && p[0]==7 && p[1]==3 && u16(p+2)==n &&
       u16(p+8)>=450){
        pthread_mutex_lock(&lock);
        unsigned remote_id=u16(p+8),function=u32(p+16);
        const fvb_device *dynamic=fvb_registry_find_remote(&registry,(uint16_t)remote_id);
        if(dynamic){
            if(!dynamic->enabled && fd==incoming_fd){provider_unlock();return (ssize_t)n;}
            if(fd!=incoming_fd){provider_unlock();return original_send(fd,buf,n,flags);}
            select_device(dynamic);remote_id=profile_remote(dynamic->profile);
            if(dynamic->hanfun.unit_id && unit_generation[dynamic-registry.devices]!=generation){
                provider_unlock();return original_send(fd,buf,n,flags);}
        }else if(!legacy_enabled || remote_id>455){provider_unlock();return original_send(fd,buf,n,flags);}

        DEBUG("provider: command remote=%u function=%u bytes=%zu\n",remote_id,function,n);
        if(debug_enabled&&fd==incoming_fd&&remote_id==455){
            fprintf(stderr,"provider: thermostat wire");
            for(size_t i=16;i<n;i++)fprintf(stderr," %02x",p[i]);
            fputc('\n',stderr);
        }
        if(fd==incoming_fd && remote_id==455 && function==55 && n>=32 &&
           u32(p+12)==n-16 && u16(p+20)==n-24){
            /* A FRITZ!App/GUI edit is never authoritative.  Consume it and
             * immediately restore the HA-owned shadow instead of forwarding
             * an ambiguous schedule mutation into Home Assistant. */
            DEBUG("provider: rejected FRITZ schedule edit, restoring HA shadow\n");
            thermostat_schedule_reject_until=(uint32_t)time(0)+2;
            emit_thermostat_values();emit_thermostat_schedule();
            provider_unlock();return n;
        }
        if(fd==incoming_fd && remote_id==455 && function==57 && n==40 &&
           u32(p+12)==24 && u16(p+20)==16){
            if(thermostat_schedule_reject_until>=(uint32_t)time(0)){
                /* FRITZ!OS follows a timer edit with a separately generated
                 * setpoint.  It belongs to the rejected edit, not to a 440
                 * button press, so keep it away from HA as well. */
                thermostat_schedule_reject_until=0;
                DEBUG("provider: rejected setpoint generated by FRITZ schedule edit\n");
                emit_thermostat_values();emit_thermostat_schedule();
                provider_unlock();return n;
            }
            unsigned target=p[24];
            if((target>=16&&target<=56)||target==253||target==254){
                thermostat_target=target;
                if(target>=16&&target<=56)thermostat_heat_target=target;
                thermostat_activated=u32(p+32);
                command_count++;last_command=target;queue_thermostat_event();
                emit_thermostat_values();provider_unlock();return n;
            }
        }
        if(fd==incoming_fd && remote_id==455 && function==117 && n==40 &&
           u32(p+12)==24 && u16(p+20)==16){
            unsigned mode=u32(p+24),previous=thermostat_timer_mode;
            uint32_t value=u32(p+28),activated=u32(p+32),end=0,duration=0;
            if(mode==0||mode==2){duration=value;end=activated+value;mode=mode==0?1:3;}
            else if(mode==1||mode==3){end=value;duration=end>activated?end-activated:0;}
            else if(mode!=255){provider_unlock();return original_send(fd,buf,n,flags);}
            thermostat_timer_mode=mode;thermostat_timer_end=mode==255?0:end;
            thermostat_timer_activated=activated;
            command_count++;last_command=mode;
            queue_thermostat_timer_event(mode,previous,thermostat_timer_end,duration);
            emit_thermostat_timer();provider_unlock();return n;
        }
        if(fd==incoming_fd && (remote_id==452||remote_id==453||remote_id==454) && function==118 && n>=36 &&
           u32(p+12)==n-16 && u16(p+20)==n-24 && u16(p+24)==1){
            unsigned inner_function=u32(p+28),inner_length=u16(p+32);
            if(n==36+inner_length && remote_id!=454 && inner_function==15 && inner_length==4 && u32(p+36)<=1){
                unsigned value=u32(p+36);
                if(remote_id==453)color_light_state=value;else hanfun_light_state=value;
                last_command=value;command_count++;queue_command_event(remote_id,0,0,0,0);
                if(remote_id==453)emit_color_relay();else emit_hanfun_relay();
                provider_unlock();return n;
            }
            if(n==36+inner_length && inner_function==110 && inner_length==8){
                unsigned value=p[36],level_type=u32(p+40);
                DEBUG("provider: HANFUN level command value=%u type=%u\n",value,level_type);
                if(remote_id==454){
                    if(level_type==0)cover_level=(value*100+127)/255;
                    else if(level_type==1)cover_level=value>100?100:value;
                    else{provider_unlock();return original_send(fd,buf,n,flags);}
                    cover_openclose_state=0;command_count++;last_command=cover_level;
                    queue_cover_event(0);emit_cover_status();
                    provider_unlock();return n;
                }
                unsigned *level=remote_id==453?&color_light_level:&hanfun_light_level;
                if(level_type==0)*level=(value*100+127)/255;
                else if(level_type==1)*level=value>100?100:value;
                else if(level_type==2)*level=(*level+(value*100+127)/255)>100?100:*level+(value*100+127)/255;
                else if(level_type==3)*level=(*level+value)>100?100:*level+value;
                else if(level_type==4){unsigned delta=(value*100+127)/255;*level=delta>*level?0:*level-delta;}
                else if(level_type==5)*level=value>*level?0:*level-value;
                command_count++;last_command=remote_id==453?color_light_state:hanfun_light_state;
                queue_command_event(remote_id,1,0,0,0);
                if(remote_id==453)emit_color_level();else emit_hanfun_level();
                provider_unlock();return n;
            }
            if(remote_id==453 && n==36+inner_length && inner_function==108 && inner_length>=4){
                unsigned mode=u32(p+36);
                if(mode==2 && inner_length==10){color_mode=2;color_temperature=u16(p+40);color_is_unmapped=0;}
                else if(!dynamic && mode==0 && inner_length==16){color_mode=0;color_hue=u16(p+40);color_saturation=p[42];color_is_unmapped=color_full?1:(p[44]?1:0);}
                else{provider_unlock();return original_send(fd,buf,n,flags);}
                command_count++;last_command=color_light_state;queue_command_event(453,0,1,0,0);
                emit_color_value();provider_unlock();return n;
            }
            if(remote_id==454 && n==36+inner_length && inner_function==125 && inner_length==8){
                unsigned action=u32(p+36);
                if(action==12||action==13||action==14){
                    cover_openclose_state=0;
                    command_count++;last_command=action;queue_cover_event(action);
                    emit_cover_openclose_status();provider_unlock();return n;
                }
            }
        }
        if(fd==incoming_fd && (remote_id==450||remote_id==451) &&
           n==28 && u32(p+12)==12 && u16(p+20)==4 &&
           (function==15 || function==25)){
            if(function==15 && u32(p+24)<=1){
                unsigned value=u32(p+24);
                if(remote_id==451)light_state=value;else switch_state=value;
                last_command=value;command_count++;
                queue_command_event(remote_id,0,0,0,0);
            }
            emit_relay(remote_id,remote_id==451?light_state:switch_state);
            provider_unlock();return n;
        }
        if(fd==incoming_fd && remote_id==451 && function==110 && n==32 &&
           u32(p+12)==16 && u16(p+20)==8){
            unsigned value=p[24],level_type=u32(p+28);
            DEBUG("provider: light level command value=%u type=%u\n",value,level_type);
            if(level_type==0)light_level=(value*100+127)/255;
            else if(level_type==1)light_level=value>100?100:value;
            else if(level_type==2)light_level=(light_level+(value*100+127)/255)>100?100:light_level+(value*100+127)/255;
            else if(level_type==3)light_level=(light_level+value)>100?100:light_level+value;
            else if(level_type==4){unsigned delta=(value*100+127)/255;light_level=delta>light_level?0:light_level-delta;}
            else if(level_type==5)light_level=value>light_level?0:light_level-value;
            command_count++;last_command=light_state;queue_command_event(451,1,0,0,0);
            /* Level commands are acknowledged by the surrounding network
             * transaction. Mirroring the function payload here feeds the
             * command back into the master and creates a command loop. */
            provider_unlock();return n;
        }
        provider_unlock();
    }
    return original_send(fd,buf,n,flags);
}
int close(int fd){
    if(!original_close)return syscall(SYS_close,fd);
    if(enabled){
        pthread_mutex_lock(&lock);
        if(fd==server_fd || fd==incoming_fd){server_fd=-1;incoming_fd=-1;announced=0;}
        for(int i=0;i<pair_count;i++)if(pairs[i][0]==fd||pairs[i][1]==fd){
            pairs[i][0]=-1;pairs[i][1]=-1;
        }
        provider_unlock();
    }return original_close(fd);
}
static void *worker(void*unused){
    (void)unused;prctl(PR_SET_NAME,"virtual_provider",0,0,0);
    DEBUG("provider: worker started\n");
    int listener=socket(AF_UNIX,SOCK_SEQPACKET,0);if(listener<0){DEBUG("provider: socket failed %d\n",errno);return 0;}
    struct sockaddr_un a={.sun_family=AF_UNIX};strcpy(a.sun_path,control_path);
    unlink(control_path);
    if(bind(listener,(struct sockaddr*)&a,sizeof(a))||chmod(control_path,0600)||listen(listener,4)){
        DEBUG("provider: control listener failed %d at %s\n",errno,control_path);
        original_close(listener);return 0;
    }
    int watchers[4]={-1,-1,-1,-1};
    struct pollfd pf[2]={{listener,POLLIN,0},{event_pipe[0],POLLIN,0}};
    for(;;){
        pthread_mutex_lock(&lock);
        if(server_fd>=0 && announced!=generation){
            if(emit_all())announced=generation;
        }
        provider_unlock();
        if(poll(pf,2,1000)<=0)continue;
        if(pf[1].revents&POLLIN){
            unsigned char wake[32];while(read(event_pipe[0],wake,sizeof(wake))>0){}
            for(;;){
                struct command_event ev={0};int have=0;
                pthread_mutex_lock(&lock);
                if(event_read!=event_write){ev=event_queue[event_read];event_read=(event_read+1)%32;have=1;}
                provider_unlock();
                if(!have)break;
                char message[224];
                int length;
                if(ev.has_thermostat_timer&&ev.thermostat_timer_mode==255)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"timer_action\":\"cancel\",\"timer_previous\":\"%s\",\"sequence\":%u}\n",
                    ev.endpoint,ev.thermostat_timer_previous==1?"boost":(ev.thermostat_timer_previous==3?"cold":"none"),ev.sequence);
                else if(ev.has_thermostat_timer)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"timer_action\":\"%s\",\"end_time\":%u,\"duration\":%u,\"sequence\":%u}\n",
                    ev.endpoint,ev.thermostat_timer_mode==1?"boost":"cold",ev.thermostat_timer_end,ev.thermostat_timer_duration,ev.sequence);
                else if(ev.has_thermostat&&ev.target_temperature==253)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"hvac_mode\":\"off\",\"sequence\":%u}\n",
                    ev.endpoint,ev.sequence);
                else if(ev.has_thermostat&&ev.target_temperature==254)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"hvac_mode\":\"heat\",\"sequence\":%u}\n",
                    ev.endpoint,ev.sequence);
                else if(ev.has_thermostat)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"hvac_mode\":\"heat\",\"target_temperature\":%u.%u,\"sequence\":%u}\n",
                    ev.endpoint,ev.target_temperature/2,(ev.target_temperature%2)*5,ev.sequence);
                else if(ev.has_cover)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"action\":\"%s\",\"position\":%u,\"sequence\":%u}\n",
                    ev.endpoint,cover_action_name(ev.cover_action),ev.level,ev.sequence);
                else if(ev.has_color&&ev.color_mode==2)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"state\":%u,\"color_mode\":\"temperature\",\"color_temperature\":%u,\"sequence\":%u}\n",
                    ev.endpoint,ev.state,ev.color_temperature,ev.sequence);
                else if(ev.has_color)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"state\":%u,\"color_mode\":\"hs\",\"hue\":%u,\"saturation\":%u,\"sequence\":%u}\n",
                    ev.endpoint,ev.state,ev.hue,ev.saturation,ev.sequence);
                else if(ev.has_level)length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"state\":%u,\"level\":%u,\"sequence\":%u}\n",
                    ev.endpoint,ev.state,ev.level,ev.sequence);
                else length=snprintf(message,sizeof(message),
                    "{\"event\":\"command\",\"endpoint\":\"%s\",\"state\":%u,\"sequence\":%u}\n",
                    ev.endpoint,ev.state,ev.sequence);
                for(unsigned i=0;i<4;i++)if(watchers[i]>=0 &&
                   original_send(watchers[i],message,length,MSG_DONTWAIT|MSG_NOSIGNAL)!=length){
                    original_close(watchers[i]);watchers[i]=-1;
                }
            }
        }
        if(!(pf[0].revents&POLLIN))continue;
        int c=accept(listener,0,0);if(c<0)continue;
        struct pollfd cp={c,POLLIN,0};char cmd[256]={0},reply[2048];
        int keep=0;
        if(poll(&cp,1,1000)>0){
            ssize_t n=recv(c,cmd,sizeof(cmd)-1,MSG_TRUNC);
            if(n>0 && (size_t)n<sizeof(cmd)){
                if(memchr(cmd,0,(size_t)n)){original_close(c);continue;}
                while(n>0&&(cmd[n-1]=='\r'||cmd[n-1]=='\n'))cmd[--n]=0;
                if(strpbrk(cmd,"\r\n")){original_close(c);continue;}
                pthread_mutex_lock(&lock);
                char requested_uid[20]={0};
                (void)sscanf(cmd,"%*s %19s",requested_uid);
                int ok=1;
                int dynamic_result=dynamic_control(cmd,sizeof(cmd));
                if(dynamic_result>=0)ok=dynamic_result;
                else if(!legacy_enabled && !selected_device && strcmp(cmd,"GET") && strcmp(cmd,"WATCH") && strcmp(cmd,"ANNOUNCE"))ok=0;
                else if(!strcmp(cmd,"SET 0")||!strcmp(cmd,"SET 1")){switch_state=cmd[4]-'0';ok=emit_relay(450,switch_state);}
                else if(!strncmp(cmd,"SET VIRT000000000001 ",21) && (cmd[21]=='0'||cmd[21]=='1') && !cmd[22]){switch_state=cmd[21]-'0';ok=emit_relay(450,switch_state);}
                else if(!strncmp(cmd,"SET VIRT000000000002 ",21) && (cmd[21]=='0'||cmd[21]=='1') && !cmd[22]){light_state=cmd[21]-'0';ok=emit_relay(451,light_state);}
                else if(!strncmp(cmd,"SET VIRT000000000003 ",21) && (cmd[21]=='0'||cmd[21]=='1') && !cmd[22]){hanfun_light_state=cmd[21]-'0';ok=emit_hanfun_relay();}
                else if(!strncmp(cmd,"SET VIRT000000000004 ",21) && (cmd[21]=='0'||cmd[21]=='1') && !cmd[22]){color_light_state=cmd[21]-'0';ok=emit_color_relay();}
                else if(!strncmp(cmd,"LEVEL VIRT000000000002 ",23)){
                    char *end=0;unsigned long value=strtoul(cmd+23,&end,10);
                    if(end==cmd+23||*end||value>100)ok=0;
                    else{light_level=(unsigned)value;ok=emit_light_level();}
                }
                else if(!strncmp(cmd,"LEVEL VIRT000000000003 ",23)){
                    char *end=0;unsigned long value=strtoul(cmd+23,&end,10);
                    if(end==cmd+23||*end||value>100)ok=0;
                    else{hanfun_light_level=(unsigned)value;ok=emit_hanfun_level();}
                }
                else if(!strncmp(cmd,"LEVEL VIRT000000000004 ",23)){
                    char *end=0;unsigned long value=strtoul(cmd+23,&end,10);
                    if(end==cmd+23||*end||value>100)ok=0;
                    else{color_light_level=(unsigned)value;ok=emit_color_level();}
                }
                else if(!strncmp(cmd,"COLOR_TEMP VIRT000000000004 ",28)){
                    char *end=0;unsigned long value=strtoul(cmd+28,&end,10);
                    if(end==cmd+28||*end||value>65535)ok=0;
                    else{color_mode=2;color_temperature=(unsigned)value;color_is_unmapped=0;ok=emit_color_value();}
                }
                else if(!strncmp(cmd,"HS VIRT000000000004 ",20)){
                    char *end=0;unsigned long hue=strtoul(cmd+20,&end,10);
                    if(end==cmd+20||*end!=' '||hue>=360)ok=0;
                    else{
                        char *sat_end=0;unsigned long saturation=strtoul(end+1,&sat_end,10);
                        if(sat_end==end+1||*sat_end||saturation>255)ok=0;
                        else{color_mode=0;color_hue=(unsigned)hue;color_saturation=(unsigned)saturation;color_is_unmapped=1;ok=emit_color_value();}
                    }
                }
                else if(!strncmp(cmd,"POSITION VIRT000000000005 ",26)){
                    char *end=0;unsigned long value=strtoul(cmd+26,&end,10);
                    if(end==cmd+26||*end||value>100)ok=0;
                    else{cover_level=100-(unsigned)value;cover_openclose_state=0;ok=emit_cover_status();}
                }
                else if(!strncmp(cmd,"TARGET VIRT000000000006 ",24)){
                    char *end=0;double value=strtod(cmd+24,&end);
                    double scaled=value*2.0;unsigned half=isfinite(value)&&value>=8.0&&value<=28.0?(unsigned)(scaled+0.5):0;
                    double difference=scaled-(double)half;if(difference<0)difference=-difference;
                    if(end==cmd+24||*end||!isfinite(value)||value<8.0||value>28.0||difference>0.01)ok=0;
                    else{thermostat_target=thermostat_heat_target=half;thermostat_activated=(uint32_t)time(0);ok=emit_thermostat_values();}
                }
                else if(!strcmp(cmd,"MODE VIRT000000000006 off")){
                    thermostat_target=253;thermostat_activated=(uint32_t)time(0);ok=emit_thermostat_values();
                }
                else if(!strcmp(cmd,"MODE VIRT000000000006 heat")){
                    thermostat_target=thermostat_heat_target;thermostat_activated=(uint32_t)time(0);ok=emit_thermostat_values();
                }
                else if(!strncmp(cmd,"TIMER VIRT000000000006 ",23)){
                    const char *value=cmd+23;
                    if(!strcmp(value,"cancel")){
                        thermostat_timer_mode=255;thermostat_timer_end=0;
                        thermostat_timer_activated=(uint32_t)time(0);ok=emit_thermostat_timer();
                    }else{
                        unsigned mode;
                        if(!strncmp(value,"boost ",6)){mode=1;value+=6;}
                        else if(!strncmp(value,"cold ",5)){mode=3;value+=5;}
                        else{mode=0;ok=0;}
                        if(ok){char *end=0;unsigned long timestamp=strtoul(value,&end,10);
                            if(end==value||*end||!timestamp||timestamp>UINT32_MAX)ok=0;
                            else{thermostat_timer_mode=mode;thermostat_timer_end=(uint32_t)timestamp;
                                thermostat_timer_activated=(uint32_t)time(0);ok=emit_thermostat_timer();}
                        }
                    }
                }
                else if(!strcmp(cmd,"SCHEDULE VIRT000000000006 disabled")){
                    thermostat_schedule_enabled=0;ok=emit_thermostat_schedule();
                }
                else if(!strncmp(cmd,"SCHEDULE VIRT000000000006 active ",33)){
                    unsigned current,next,current_minute,next_minute;char tail;
                    if(sscanf(cmd+33,"%u %u %u %u%c",&current,&next,&current_minute,&next_minute,&tail)!=4 ||
                       current<16||current>56||next<16||next>56||
                       current_minute>=10080||next_minute>=10080||current_minute==next_minute)ok=0;
                    else{
                        thermostat_schedule_enabled=1;
                        thermostat_schedule_current=current;thermostat_schedule_next=next;
                        thermostat_schedule_current_minute=current_minute;
                        thermostat_schedule_next_minute=next_minute;
                        thermostat_target=thermostat_heat_target=thermostat_comfort=current;
                        thermostat_reduced=next;thermostat_activated=(uint32_t)time(0);
                        ok=emit_thermostat_values()&&emit_thermostat_schedule();
                    }
                }
                else if(!strcmp(cmd,"ANNOUNCE")){ok=emit_all();}
                else if(hanfun_experiment&&!strcmp(cmd,"HANFUN UNIT")){
                    ok=emit_hanfun_unit_config();
                }
                else if(hanfun_experiment&&!strcmp(cmd,"HANFUN STATUS")){
                    ok=emit_hanfun_relay() && emit_hanfun_level();
                }
                else if(hanfun_experiment&&!strcmp(cmd,"COLOR UNIT")){
                    ok=emit_color_unit_config();
                }
                else if(hanfun_experiment&&!strcmp(cmd,"COLOR STATUS")){
                    ok=emit_color_status();
                }
                else if(hanfun_experiment&&!strcmp(cmd,"COLOR FULL")){
                    color_full=1;ok=emit_color_capabilities();
                }
                else if(hanfun_experiment&&!strcmp(cmd,"COLOR CT")){
                    color_full=0;color_mode=2;ok=emit_color_capabilities()&&emit_color_value();
                }
                else if(hanfun_experiment&&!strcmp(cmd,"BLIND UNIT")){
                    ok=emit_cover_unit_config();
                }
                else if(hanfun_experiment&&!strcmp(cmd,"BLIND STATUS")){
                    ok=emit_cover_status();
                }
                else if(!strcmp(cmd,"WATCH")){
                    ok=0;
                    for(unsigned i=0;i<4;i++)if(watchers[i]<0){watchers[i]=c;keep=ok=1;break;}
                }
                else if(strcmp(cmd,"GET"))ok=0;
                snprintf(reply,sizeof(reply),"{\"ok\":%s,\"generation\":%u,\"connected\":%s,\"state\":%u,\"light_state\":%u,\"light_level\":%u,\"hanfun_light_state\":%u,\"hanfun_light_level\":%u,\"color_light_state\":%u,\"color_light_level\":%u,\"color_full\":%u,\"color_mode\":%u,\"color_temperature\":%u,\"color_hue\":%u,\"color_saturation\":%u,\"color_is_unmapped\":%u,\"cover_level\":%u,\"cover_position\":%u,\"cover_openclose_state\":%u,\"thermostat_mode\":\"%s\",\"thermostat_target\":%u.%u,\"thermostat_actual\":%u.%u,\"thermostat_timer\":\"%s\",\"thermostat_timer_end\":%u,\"thermostat_schedule\":\"%s\",\"thermostat_schedule_current\":%u.%u,\"thermostat_schedule_next\":%u.%u,\"thermostat_schedule_current_minute\":%u,\"thermostat_schedule_next_minute\":%u,\"commands\":%u,\"last_command\":%u,\"server_fd\":%d}\n",ok?"true":"false",generation,server_fd>=0?"true":"false",switch_state,light_state,light_level,hanfun_light_state,hanfun_light_level,color_light_state,color_light_level,color_full,color_mode,color_temperature,color_hue,color_saturation,color_is_unmapped,cover_level,100-cover_level,cover_openclose_state,thermostat_target==253?"off":"heat",thermostat_heat_target/2,(thermostat_heat_target%2)*5,thermostat_actual/2,(thermostat_actual%2)*5,thermostat_timer_mode==1?"boost":(thermostat_timer_mode==3?"cold":"none"),thermostat_timer_end,thermostat_schedule_enabled?"active":"disabled",thermostat_schedule_current/2,(thermostat_schedule_current%2)*5,thermostat_schedule_next/2,(thermostat_schedule_next%2)*5,thermostat_schedule_current_minute,thermostat_schedule_next_minute,command_count,last_command,server_fd);
                const fvb_device *reply_device=fvb_registry_find(&registry,requested_uid);
                if(reply_device){
                    size_t end=strlen(reply);
                    if(end>=2)snprintf(reply+end-2,sizeof(reply)-end+2,
                        ",\"remote_id\":%u,\"profile\":\"%s\"}\n",
                        reply_device->remote_id,fvb_profile_name(reply_device->profile));
                }
                provider_unlock();
                original_send(c,reply,strlen(reply),0);
            }
        }if(!keep)original_close(c);
    }
    return 0;
}
__attribute__((constructor)) static void start(void){
    original_write=dlsym(RTLD_NEXT,"write");original_send=dlsym(RTLD_NEXT,"send");
    original_close=dlsym(RTLD_NEXT,"close");original_socketpair=dlsym(RTLD_NEXT,"socketpair");
    const char *v=getenv("AHA_VIRTUAL_LAB");if(!v||strcmp(v,"1"))return;
    const char *legacy=getenv("AHA_VIRTUAL_LEGACY");
    legacy_enabled=legacy&&!strcmp(legacy,"1");
    fvb_registry_init(&registry);
    debug_enabled=getenv("AHA_VIRTUAL_DEBUG")!=0;
    hanfun_experiment=getenv("AHA_VIRTUAL_HANFUN_EXPERIMENT")!=0;
    hanfun_timestamp=(uint32_t)time(0);
    if(!hanfun_timestamp)hanfun_timestamp=1;
    values_save(&initial_values);
    const char *p=getenv("AHA_VIRTUAL_CONTROL_PATH");if(p&&*p)control_path=p;
    if(pipe(event_pipe)){DEBUG("provider: pipe failed %d\n",errno);return;}
    fcntl(event_pipe[0],F_SETFL,fcntl(event_pipe[0],F_GETFL)|O_NONBLOCK);
    fcntl(event_pipe[1],F_SETFL,fcntl(event_pipe[1],F_GETFL)|O_NONBLOCK);
    enabled=1;pthread_t t;
    int result=pthread_create(&t,0,worker,0);DEBUG("provider: pthread_create=%d\n",result);
    if(!result)pthread_detach(t);
}
