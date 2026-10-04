// SPDX-License-Identifier: MIT OR Apache-2.0
/* Small local client for native AHA lifecycle operations.  AVM libraries are
 * loaded at runtime and are neither linked into nor distributed by the
 * project. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *load_symbol(void *handle,const char *name){
    dlerror();
    void *symbol=dlsym(handle,name);
    const char *error=dlerror();
    if(error||!symbol){fprintf(stderr,"fritzvirtual-aha-control: %s\n",error?error:"missing symbol");return NULL;}
    return symbol;
}
static int valid_uid(const char *uid){
    if(!uid||strlen(uid)!=19||memcmp(uid,"FVB",3))return 0;
    for(size_t i=3;i<19;i++)
        if(!((uid[i]>='0'&&uid[i]<='9')||(uid[i]>='A'&&uid[i]<='F')))return 0;
    return 1;
}
int main(int argc,char **argv){
    if(argc!=3||strcmp(argv[1],"delete")||!valid_uid(argv[2])){
        fprintf(stderr,"usage: %s delete FVB0123456789ABCDEF\n",argv[0]);return 2;
    }
    void *lua=dlopen("liblua.so.1",RTLD_LAZY|RTLD_GLOBAL);
    void *aha=lua?dlopen("libaha.so.1",RTLD_LAZY|RTLD_GLOBAL):NULL;
    if(!lua||!aha){fprintf(stderr,"fritzvirtual-aha-control: %s\n",dlerror());if(lua)dlclose(lua);return 3;}
    void *(*create)(void)=load_symbol(lua,"luaL_newstate");
    void (*openlibs)(void*)=load_symbol(lua,"luaL_openlibs");
    int (*openaha)(void*)=load_symbol(aha,"luaopen_libaha");
    int (*loadstring)(void*,const char*)=load_symbol(lua,"luaL_loadstring");
    int (*call)(void*,int,int,int)=load_symbol(lua,"lua_pcall");
    int (*toboolean)(void*,int)=load_symbol(lua,"lua_toboolean");
    const char *(*tostring)(void*,int,size_t*)=load_symbol(lua,"lua_tolstring");
    void (*settop)(void*,int)=load_symbol(lua,"lua_settop");
    void (*close_lua)(void*)=load_symbol(lua,"lua_close");
    if(!create||!openlibs||!openaha||!loadstring||!call||!toboolean||!tostring||!settop||!close_lua){dlclose(aha);dlclose(lua);return 3;}
    void *state=create();if(!state){dlclose(aha);dlclose(lua);return 3;}
    openlibs(state);openaha(state);
    char script[512];
    int length=snprintf(script,sizeof(script),
        "local i=aha.GetIDByAIN('%s');if i then aha.DeleteDevice(i) end",argv[2]);
    int result=length<=0||(size_t)length>=sizeof(script)||loadstring(state,script)||call(state,0,0,0);
    int absent=0;
    for(unsigned attempt=0;!result&&attempt<30;attempt++){
        length=snprintf(script,sizeof(script),
            "return aha.GetIDByAIN('%s')==nil",argv[2]);
        result=length<=0||(size_t)length>=sizeof(script)||loadstring(state,script)||call(state,0,1,0);
        if(result)break;
        absent=toboolean(state,-1);settop(state,-2);
        if(absent)break;
        usleep(100000);
    }
    if(!result&&!absent){fprintf(stderr,"fritzvirtual-aha-control: device still present\n");result=1;}
    if(result){
        const char *message=tostring(state,-1,NULL);
        if(message)fprintf(stderr,"fritzvirtual-aha-control: Lua: %s\n",message);
    }
    close_lua(state);dlclose(aha);dlclose(lua);
    return result?4:0;
}
