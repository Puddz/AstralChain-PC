/* Bounded HIPC/CMIF subset: two input pointer/buffer pairs, one output alias or
 * receive-static buffer, two copied handles and one domain input object. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t type, command, token, send_pid;
    const uint8_t *arguments;
    size_t argument_size;
    uint64_t send_address, send_size;
    uint32_t copy_count, copy_handle;
    uint32_t domain, object_id, domain_command;
    uint64_t receive_address, receive_size;
    uint32_t receive_count;
    uint32_t send_is_pointer;
    uint32_t input_object_count,input_object;
    uint32_t receive_is_pointer;
    uint32_t receive_mode;
    uint32_t copy_handle2;
    uint64_t send_address2,send_size2;
    uint32_t send_is_pointer2,send_count;
    /* Both a pointer (X) and alias (A) input at one index: the paired view keeps
     * the pointer. Dispatch rejects this unless the service reads no inputs. */
    uint32_t ambiguous_inputs;
} IpcRequest;

static uint32_t ipc_u32(const void *p) { uint32_t n; memcpy(&n,p,4); return n; }
static void ipc_put32(void *p,uint32_t n) { memcpy(p,&n,4); }

static const char *ipc_parse_mode(const uint8_t *buffer,size_t size,int domain,IpcRequest *request) {
    if(size<8) return "short HIPC header";
    uint32_t first=ipc_u32(buffer), second=ipc_u32(buffer+4);
    uint32_t type=first&0xffff, words=second&1023;
    if(type!=4 && type!=6 && type!=5 && type!=7) return "unsupported HIPC request type";
    unsigned statics=(first>>16)&15, sends=(first>>20)&15, receives=(first>>24)&15;
    unsigned receive_mode=(second>>10)&15;
    if(statics>2 || sends>2 || receives>1 || first>>28 || second&0x7fffc000 || (receive_mode && receive_mode!=3)) return "unsupported HIPC descriptors/flags";
    size_t raw=8;
    uint32_t pid=0;
    uint32_t copy_count=0,copy_handle=0,copy_handle2=0;
    if(second>>31) {
        if(size<12) return "short special header";
        uint32_t special=ipc_u32(buffer+8);
        if(special&~31u) return "unsupported HIPC handles/flags";
        pid=special&1;
        copy_count=(special>>1)&15;
        if(copy_count>2) return "unsupported HIPC copy count";
        raw+=4+8*pid;
        if(copy_count) {
            if(raw+4*copy_count>size) return "short copy handle";
            copy_handle=ipc_u32(buffer+raw);raw+=4;
            if(copy_count==2) {copy_handle2=ipc_u32(buffer+raw);raw+=4;}
        }
    }
    if(raw+(size_t)statics*8+(size_t)(sends+receives)*12>size) return "short buffer descriptors";
    uint64_t addresses[2]={0},lengths[2]={0};
    unsigned pointers[2]={0},send_count=statics>sends ? statics : sends,ambiguous=0;
    for(unsigned i=0;i<statics;i++) {
        uint32_t a=ipc_u32(buffer+raw),b=ipc_u32(buffer+raw+4);raw+=8;
        addresses[i]=b|((uint64_t)((a>>12)&15)<<32)|((uint64_t)((a>>6)&63)<<36);
        lengths[i]=a>>16;pointers[i]=1;
    }
    for(unsigned i=0;i<sends;i++) {
        uint32_t a=ipc_u32(buffer+raw),b=ipc_u32(buffer+raw+4),d=ipc_u32(buffer+raw+8);raw+=12;
        uint64_t length=a|((uint64_t)((d>>24)&15)<<32);
        if(lengths[i] && length) {ambiguous=1;continue;}
        if((d&3)>1) return "unsupported input buffer mode";
        if(length) {
            addresses[i]=b|((uint64_t)(d>>28)<<32)|((uint64_t)((d>>2)&0x3fffff)<<36);
            lengths[i]=length;pointers[i]=0;
        }
    }
    for(unsigned i=0;i<send_count;i++)
        if(addresses[i]>=(1ull<<39) || lengths[i]>(1ull<<39)-addresses[i]) return "input buffer outside guest address space";
    uint64_t address=addresses[0],length=lengths[0];unsigned send_is_pointer=pointers[0];
    uint64_t receive_address=0,receive_size=0;unsigned alias_mode=0;
    if(receives) {
        uint32_t a=ipc_u32(buffer+raw),b=ipc_u32(buffer+raw+4),d=ipc_u32(buffer+raw+8);raw+=12;
        alias_mode=d&3;
        if(alias_mode>1) return "unsupported output buffer mode";
        receive_address=b|((uint64_t)(d>>28)<<32)|((uint64_t)((d>>2)&0x3fffff)<<36);
        receive_size=a|((uint64_t)((d>>24)&15)<<32);
        if(receive_address>=(1ull<<39) || receive_size>(1ull<<39)-receive_address)
            return "output buffer outside guest address space";
    }
    size_t aligned=(raw+15)&~(size_t)15, end=raw+(size_t)words*4;
    if(end>size || aligned>end || end-aligned<16) return "CMIF extent outside message";
    unsigned receive_is_pointer=0;
    if(receive_mode==3) {
        if(size-end<8) return "short receive-static descriptor";
        uint32_t low=ipc_u32(buffer+end),high=ipc_u32(buffer+end+4);
        uint64_t pointer_address=low|((uint64_t)(high&65535)<<32),pointer_size=high>>16;
        if(pointer_address>=(1ull<<39) || pointer_size>(1ull<<39)-pointer_address)
            return "output pointer outside guest address space";
        if(receive_size && pointer_size) return "ambiguous output buffers";
        if(!receive_size) {
            receive_address=pointer_address;receive_size=pointer_size;receive_is_pointer=1;receives=1;
        }
    }
    uint32_t object_id=0,domain_command=0,domain_token=0,input_object_count=0,input_object=0;
    domain=domain && (type==4 || type==6);
    if(domain) {
        uint32_t header=ipc_u32(buffer+aligned),data_size=header>>16;
        domain_command=header&255;object_id=ipc_u32(buffer+aligned+4);domain_token=ipc_u32(buffer+aligned+12);
        input_object_count=(header>>8)&255;
        if(input_object_count>1 || ipc_u32(buffer+aligned+8)) return "unsupported domain input objects/flags";
        if(domain_command==2) {
            if(data_size || input_object_count) return "domain close has payload";
            *request=(IpcRequest){.type=type,.token=domain_token,.domain=1,.object_id=object_id,.domain_command=2};
            return NULL;
        }
        if(domain_command!=1 || data_size<16 || data_size>end-aligned-16) return "invalid domain payload extent/type";
        if(input_object_count) {
            if(end-aligned-16-data_size<4) return "domain input object outside message";
            input_object=ipc_u32(buffer+aligned+16+data_size);
            if(!input_object) return "invalid domain input object";
        }
        aligned+=16;end=aligned+data_size;
    }
    if(ipc_u32(buffer+aligned)!=0x49434653) return "missing SFCI";
    if(ipc_u32(buffer+aligned+4)>1) return "unsupported CMIF version";
    *request=(IpcRequest){type,ipc_u32(buffer+aligned+8),ipc_u32(buffer+aligned+12),pid,
                         buffer+aligned+16,end-aligned-16,address,length,copy_count,copy_handle,(uint32_t)domain,object_id,domain_command};
    if(domain) request->token=domain_token;
    request->receive_address=receive_address;request->receive_size=receive_size;request->receive_count=receives;
    request->send_is_pointer=send_is_pointer;
    request->input_object_count=input_object_count;request->input_object=input_object;
    request->receive_is_pointer=receive_is_pointer;
    request->copy_handle2=copy_handle2;
    request->receive_mode=alias_mode;
    request->send_address2=addresses[1];request->send_size2=lengths[1];
    request->send_is_pointer2=pointers[1];request->send_count=send_count;request->ambiguous_inputs=ambiguous;
    return NULL;
}

static const char *ipc_parse(const uint8_t *buffer,size_t size,IpcRequest *request) {
    return ipc_parse_mode(buffer,size,0,request);
}

static int ipc_reply_domain(uint8_t *buffer,size_t size,uint32_t result,uint32_t token,
                            const void *payload,size_t payload_size,uint32_t object) {
    if(payload_size>128) return 0;
    size_t aligned=(payload_size+3)&~(size_t)3, total=56+aligned+(object ? 4 : 0);
    if(total>size) return 0;
    memset(buffer,0,total);
    ipc_put32(buffer+4,(uint32_t)((total-8)/4));
    ipc_put32(buffer+16,object ? 1 : 0);
    ipc_put32(buffer+32,0x4f434653);ipc_put32(buffer+40,result);ipc_put32(buffer+44,token);
    if(payload_size) memcpy(buffer+48,payload,payload_size);
    if(object) ipc_put32(buffer+48+aligned,object);
    return 1;
}

static int ipc_reply_handle(uint8_t *buffer,size_t size,uint32_t handle,uint32_t token) {
    if(size<48) return 0;
    memset(buffer,0,48);
    ipc_put32(buffer+4,0x80000008);
    ipc_put32(buffer+8,32); /* one move handle */
    ipc_put32(buffer+12,handle);
    ipc_put32(buffer+16,0x4f434653);
    ipc_put32(buffer+28,token);
    return 1;
}

static int ipc_reply(uint8_t *buffer,size_t size,uint32_t result,uint32_t token) {
    if(size<40) return 0;
    memset(buffer,0,40);
    ipc_put32(buffer+4,8);
    ipc_put32(buffer+16,0x4f434653);
    ipc_put32(buffer+24,result);
    ipc_put32(buffer+28,token);
    return 1;
}

static int ipc_reply_u32(uint8_t *buffer,size_t size,uint32_t value,uint32_t token) {
    if(size<44) return 0;
    memset(buffer,0,44);
    ipc_reply(buffer,size,0,token);
    ipc_put32(buffer+4,9);
    ipc_put32(buffer+32,value);
    return 1;
}

static int ipc_reply_u64(uint8_t *buffer,size_t size,uint64_t value,uint32_t token) {
    if(size<48) return 0;
    memset(buffer,0,48);
    ipc_reply(buffer,size,0,token);
    ipc_put32(buffer+4,10);
    memcpy(buffer+32,&value,8);
    return 1;
}

/* Success with raw output data (word-padded), no handles or objects. */
static int ipc_reply_data(uint8_t *buffer,size_t size,const void *data,size_t data_size,uint32_t token) {
    size_t aligned=(data_size+3)&~(size_t)3;
    if(data_size>128 || 32+aligned>size) return 0;
    memset(buffer,0,32+aligned);
    ipc_reply(buffer,size,0,token);
    ipc_put32(buffer+4,(uint32_t)(8+aligned/4));
    memcpy(buffer+32,data,data_size);
    return 1;
}

static int ipc_reply_copy(uint8_t *buffer,size_t size,uint32_t handle,int domain,uint32_t token) {
    size_t required=domain ? 64 : 48;
    if(size<required) return 0;
    memset(buffer,0,required);
    if(domain) ipc_reply_domain(buffer,size,0,token,NULL,0,0);
    else ipc_reply_handle(buffer,size,handle,token);
    ipc_put32(buffer+4,0x80000000u|(domain ? 12 : 8));
    ipc_put32(buffer+8,2);ipc_put32(buffer+12,handle);
    return 1;
}
