/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/* Host test: compile with -ffunction-sections -Wl,--gc-sections, cJSON and
 * device_registry.c/registry_store.c. No broker or live Provider is used. */
#define main mqtt_bridge_program_main
#include "../freetz/package/src/mqtt_bridge.c"
#undef main
#include <assert.h>

static char published_response[16384];
int mosquitto_publish(struct mosquitto *client,int *mid,const char *topic,int length,const void *payload,int qos,bool retain){
    (void)client;(void)mid;(void)qos;(void)retain;
    if(strstr(topic,"/management/response/")){
        assert(length<(int)sizeof(published_response));memcpy(published_response,payload,(size_t)length);published_response[length]=0;
    }
    return MOSQ_ERR_SUCCESS;
}
static const char *candidate(const char *json,fvb_device_registry *r,int *mutate,int *announce){
    cJSON *root=cJSON_Parse(json);assert(root);
    const char *error=management_candidate(root,r,mutate,announce);cJSON_Delete(root);return error;
}
static void management(const char *payload){
    struct mosquitto_message message={.topic=topic_management,.payload=(void*)payload,.payloadlen=(int)strlen(payload)};
    management_locked(&message);
}
static void feedback(const char *uid,const char *kind,const char *value){
    char topic[512];snprintf(topic,sizeof(topic),"%s/device/%s/%s/set",config.topic_prefix,uid,kind);
    struct mosquitto_message message={.topic=topic,.payload=(void*)value,.payloadlen=(int)strlen(value)};
    device_message_locked(NULL,NULL,&message);
}
/* Local fake Provider validates identity checks and delayed provisioning;
 * it never opens a network socket or contacts an AVM daemon. */
static pid_t fake_provider(const char *path){
    int listener=socket(AF_UNIX,SOCK_SEQPACKET,0);assert(listener>=0);
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    assert(!bind(listener,(struct sockaddr*)&address,sizeof(address))&&!listen(listener,8));
    pid_t child=fork();assert(child>=0);
    if(!child){
        unsigned id=456,renamed=0,units=0;int conflict=0;
        for(;;){
            int fd=accept(listener,NULL,NULL);if(fd<0)_exit(2);
            char command[256],reply[512];ssize_t n=recv(fd,command,sizeof(command)-1,0);
            if(n<=0){close(fd);continue;}command[n]=0;
            if(!strncmp(command,"RESTORE ",8)){
                char uid[20];unsigned restored;
                assert(sscanf(command,"RESTORE %19s %u",uid,&restored)==2);
                assert(restored==456);
            }
            if(!strcmp(command,"TEST WRONG_ID"))id=999;
            if(!strcmp(command,"TEST NAME_CONFLICT")){id=456;conflict=1;}
            if(!strncmp(command,"RENAME ",7)){conflict=0;renamed++;}
            if(!strncmp(command,"UNIT ",5))units++;
            snprintf(reply,sizeof(reply),"{\"ok\":%s,\"connected\":true,\"generation\":7,\"remote_id\":%u,\"profile\":\"color_temperature_light\",\"renamed\":%u,\"units\":%u}",
                conflict&&!strncmp(command,"RESTORE ",8)?"false":"true",id,renamed,units);
            (void)send(fd,reply,strlen(reply),MSG_NOSIGNAL);close(fd);
        }
    }
    close(listener);return child;
}
int main(void){
    assert(identifier_valid("bridge_7530-1",32)&&!identifier_valid("bridge/7530",32));
    assert(!identifier_valid("ab",32)&&!identifier_valid("Bridge",32));
    assert(!identifier_valid("123456789012345678901234567890123",32));
    assert(prefix_valid("fritzvirtual/bridge_7530-1"));
    assert(!prefix_valid("/bridge")&&!prefix_valid("bridge/")&&!prefix_valid("bridge//device")&&!prefix_valid("bridge/+"));
    int mutate,announce;fvb_device_registry r;fvb_registry_init(&r);
    const char *add="{\"schema_version\":1,\"request_id\":\"test-1\",\"operation\":\"upsert_device\",\"device\":{\"uid\":\"FVB0123456789ABCDEF\",\"name\":\"Licht Küche\",\"profile\":\"color_temperature_light\",\"enabled\":true}}";
    assert(!candidate(add,&r,&mutate,&announce));assert(mutate&&r.count==1&&r.devices[0].remote_id==456);
    uint64_t revision=r.revision;
    assert(!candidate(add,&r,&mutate,&announce));assert(r.revision==revision);
    assert(candidate("{\"schema_version\":2,\"operation\":\"list_devices\"}",&r,&mutate,&announce));
    assert(candidate("{\"schema_version\":1,\"operation\":\"delete_device\"}",&r,&mutate,&announce));
    assert(candidate("{\"schema_version\":1,\"schema_version\":1,\"operation\":\"list_devices\"}",&r,&mutate,&announce));
    assert(candidate("{\"schema_version\":1,\"operation\":\"rename_device\",\"expected_revision\":0,\"uid\":\"FVB0123456789ABCDEF\",\"name\":\"bad\"}",&r,&mutate,&announce));
    assert(candidate("{\"schema_version\":1,\"operation\":\"upsert_device\",\"device\":{\"uid\":\"FVB0123456789ABCDEF\",\"name\":\"bad\",\"profile\":\"switch\"}}",&r,&mutate,&announce));
    assert(candidate("{\"schema_version\":1,\"operation\":\"reconcile_devices\",\"enabled_uids\":[\"FVB0123456789ABCDEF\",\"FVB0123456789abcdef\"]}",&r,&mutate,&announce));
    assert(!candidate("{\"schema_version\":1,\"operation\":\"reconcile_devices\",\"enabled_uids\":[]}",&r,&mutate,&announce));
    assert(!r.devices[0].enabled&&r.count==1);
    assert(!candidate(add,&r,&mutate,&announce));assert(r.devices[0].enabled);
    fvb_device_registry pruned=r;
    assert(!candidate("{\"schema_version\":1,\"operation\":\"prune_devices\",\"keep_uids\":[]}",&pruned,&mutate,&announce));
    assert(mutate&&pruned.count==0&&pruned.next_remote_id==457);
    assert(!request_id_valid("../../bad")&&!request_id_valid("bad/+"));assert(request_id_valid("id-012_abc"));
    cJSON *nan=cJSON_CreateNumber(NAN);uint64_t integer;assert(!json_integer(nan,&integer));cJSON_Delete(nan);
    registry=r;provider_ready=0;
    feedback(r.devices[0].uid,"level","55");assert(strstr(feedback_cache[0][1],"55"));
    feedback(r.devices[0].uid,"level","101");assert(strstr(feedback_cache[0][1],"55"));
    feedback(r.devices[0].uid,"position","40");assert(!*feedback_cache[0][3]);
    feedback(r.devices[0].uid,"target_temperature","nan");assert(!*feedback_cache[0][4]);
    feedback(r.devices[0].uid,"color_temperature","2700");assert(strstr(feedback_cache[0][2],"2700"));
    assert(fvb_registry_add(&registry,registry.revision,"FVB1123456789ABCDEF","Thermo",FVB_PROFILE_THERMOSTAT,NULL)==FVB_REGISTRY_OK);
    feedback(registry.devices[1].uid,"target_temperature","nan");assert(!*feedback_cache[1][4]);
    feedback(registry.devices[1].uid,"target_temperature","20.5");assert(strstr(feedback_cache[1][4],"20.5"));
    feedback(registry.devices[1].uid,"target_temperature","20.2");assert(strstr(feedback_cache[1][4],"20.5"));
    feedback(registry.devices[1].uid,"timer","boost 4294967296");assert(!*feedback_cache[1][6]);
    feedback(registry.devices[1].uid,"schedule","active 40 36 50 60");assert(strstr(feedback_cache[1][7],"active"));
    char directory[]="/tmp/fvb-mqtt-test-XXXXXX";assert(mkdtemp(directory));
    snprintf(config.registry_file,sizeof(config.registry_file),"%s/registry.json",directory);
    strcpy(config.control_socket,"/tmp/nonexistent-fvb-mqtt-test.sock");config.persist_command[0]=0;strcpy(config.bridge_id,"test");assert(build_topics());
    fvb_registry_init(&registry);
    management(add);
    assert(registry.count==1);
    cJSON *response=cJSON_Parse(published_response);assert(cJSON_IsTrue(cJSON_GetObjectItem(response,"accepted")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(response,"ready")));cJSON_Delete(response);
    char error[256];fvb_device_registry loaded;
    assert(fvb_registry_load_file(config.registry_file,&loaded,error,sizeof(error))&&loaded.count==1);
    revision=registry.revision;
    management(add);assert(registry.revision==revision);
    strcpy(config.registry_file,"/this/path/does/not/exist/registry.json");
    management("{\"schema_version\":1,\"request_id\":\"rename\",\"operation\":\"rename_device\",\"uid\":\"FVB0123456789ABCDEF\",\"name\":\"Changed\"}");
    assert(registry.revision==revision&&!strcmp(registry.devices[0].name,"Licht Küche"));
    response=cJSON_Parse(published_response);assert(cJSON_IsFalse(cJSON_GetObjectItem(response,"accepted")));cJSON_Delete(response);
    snprintf(config.registry_file,sizeof(config.registry_file),"%s/registry.json",directory);
    strcpy(config.persist_command,"/bin/false");
    const char *rename="{\"schema_version\":1,\"request_id\":\"rename\",\"operation\":\"rename_device\",\"uid\":\"FVB0123456789ABCDEF\",\"name\":\"Changed\"}";
    management(rename);assert(flash_dirty&&!strcmp(registry.devices[0].name,"Changed"));
    response=cJSON_Parse(published_response);assert(cJSON_IsTrue(cJSON_GetObjectItem(response,"accepted")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(response,"flash_persisted")));cJSON_Delete(response);
    strcpy(config.persist_command,"/bin/true");management(rename);assert(!flash_dirty);
    snprintf(config.control_socket,sizeof(config.control_socket),"%s/provider.sock",directory);
    pid_t server=fake_provider(config.control_socket);
    assert(reconcile_provider_locked()&&!provider_ready&&unit_due>time(NULL));
    assert(!finish_units_locked());
    unit_due=time(NULL)-1;assert(finish_units_locked()&&provider_ready);
    cJSON *status=provider_query("GET");assert(cJSON_GetObjectItem(status,"units")->valueint==1);cJSON_Delete(status);
    assert(provider_request("TEST WRONG_ID"));assert(!reconcile_provider_locked()&&!provider_ready);
    assert(provider_request("TEST NAME_CONFLICT"));assert(reconcile_provider_locked());
    status=provider_query("GET");assert(cJSON_GetObjectItem(status,"renamed")->valueint==1);cJSON_Delete(status);
    strcpy(config.aha_control,"/bin/true");
    management("{\"schema_version\":1,\"request_id\":\"prune\",\"operation\":\"prune_devices\",\"keep_uids\":[]}");
    assert(registry.count==0&&registry.next_remote_id==457);
    response=cJSON_Parse(published_response);assert(cJSON_IsTrue(cJSON_GetObjectItem(response,"accepted")));cJSON_Delete(response);
    assert(fvb_registry_load_file(config.registry_file,&loaded,error,sizeof(error))&&loaded.count==0&&loaded.next_remote_id==457);
    kill(server,SIGTERM);assert(waitpid(server,NULL,0)==server);unlink(config.control_socket);
    char file[512];snprintf(file,sizeof(file),"%s/registry.json",directory);unlink(file);rmdir(directory);
    puts("mqtt_bridge_test: OK");return 0;
}
