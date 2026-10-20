/* Guest GM20B ABI profile, not host GPU capabilities. Command execution still
 * stops at unimplemented operations. Layout/values: libnx nvidia/ioctl.h. */
typedef struct {
 uint32_t arch,implementation,revision,gpc_count;
 uint64_t l2_size,video_memory_size;
 uint32_t tpc_per_gpc,bus_type,big_page_size,compression_page_size;
 uint32_t pde_bits,big_page_sizes,gpc_mask,sm_version,spa_version,warp_count,va_bits,reserved;
 uint64_t flags;
 uint32_t class_2d,class_3d,class_compute,class_gpfifo,class_inline,class_dma;
 uint32_t fbp_count,fbp_mask,ltc_per_fbp,lts_per_ltc,tex_per_tpc,max_gpc,rop_mask[2];
 uint64_t chip_name,compbit_base;
} GpuCharacteristics;
_Static_assert(sizeof(GpuCharacteristics)==160,"GPU characteristics wire size");
_Static_assert(offsetof(GpuCharacteristics,flags)==80,"GPU flags wire offset");
_Static_assert(offsetof(GpuCharacteristics,chip_name)==144,"GPU name wire offset");
static const GpuCharacteristics gpu_model={
 .arch=0x120,.implementation=0xb,.revision=0xa1,.gpc_count=1,
 .l2_size=0x40000,.tpc_per_gpc=2,.bus_type=0x20,
 .big_page_size=0x20000,.compression_page_size=0x20000,.pde_bits=27,
 .big_page_sizes=0x30000,.gpc_mask=1,.sm_version=0x503,.spa_version=0x503,
 .warp_count=0x80,.va_bits=40,.flags=0x55,
 .class_2d=0x902d,.class_3d=0xb197,.class_compute=0xb1c0,
 .class_gpfifo=0xb06f,.class_inline=0xa140,.class_dma=0xb0b5,
 .fbp_count=1,.ltc_per_fbp=2,.lts_per_ltc=1,.max_gpc=1,
 .rop_mask={0x21d70,0},.chip_name=0x6230326d67ull
};
/* Compatibility context size used by Ryujinx, not a captured hardware value.
 * Software binding retains mapped storage; hardware save/restore is unfinished. */
static const uint32_t gpu_zcull_context_size=1;
static const uint32_t gpu_zcull_info[10]={32,32,1024,2048,32,32,192,32,64,16};
/* Compatibility profile from libnx's slot observation and Ryujinx's response.
 * The precise slot/mask semantics are not hardware-verified. This is not a
 * count of the software clear-value registrations in gpu_zbc.h. */
static const uint32_t gpu_zbc_slot_profile[2]={7,1};
static void nv_get_zbc_slots(uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size || request->receive_count!=1 || request->receive_size<8)
  boundary("nv-zbc-slots-layout",&state,state.pc);
 Region target=query(request->receive_address);
 if(!(target.permission&2) || 8>target.size-(request->receive_address-target.base))
  boundary("nv-zbc-slots-memory",&state,state.pc);
 memcpy((void*)(uintptr_t)request->receive_address,gpu_zbc_slot_profile,8);
 ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_get_zcull(uint8_t *buffer,const IpcRequest *request,int info) {
 size_t size=info ? sizeof(gpu_zcull_info) : sizeof(gpu_zcull_context_size);
 if(request->send_size || request->receive_count!=1 || request->receive_size<size)
  boundary("nv-zcull-layout",&state,state.pc);
 Region target=query(request->receive_address);
 if(!(target.permission&2) || size>target.size-(request->receive_address-target.base))
  boundary("nv-zcull-memory",&state,state.pc);
 memcpy((void*)(uintptr_t)request->receive_address,info ? (const void*)gpu_zcull_info : (const void*)&gpu_zcull_context_size,size);
 ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_get_characteristics(uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=176 || request->receive_count!=1 || request->receive_size<176)
  boundary("nv-characteristics-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || 176>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || 176>target.size-(request->receive_address-target.base))
  boundary("nv-characteristics-memory",&state,state.pc);
 /* Snapshot before writing: input and output can alias, including partially. */
 uint8_t result[176];memcpy(result,(void*)(uintptr_t)request->send_address,sizeof(result));
 uint64_t size,address;memcpy(&size,result,8);memcpy(&address,result+8,8);
 printf("NV characteristics size=0x%llx address=0x%llx\n",(unsigned long long)size,(unsigned long long)address);
 if(!size || !address) {ipc_reply_u32(buffer,256,4,request->token);return;}
 if(size<sizeof(gpu_model)) boundary("nv-characteristics-partial",&state,state.pc);
 size=sizeof(gpu_model);memcpy(result,&size,8);memcpy(result+16,&gpu_model,sizeof(gpu_model));
 /* The pointer is an opaque compatibility field; output is inline. */
 memcpy((void*)(uintptr_t)request->receive_address,result,sizeof(result));
 ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_get_tpc_masks(uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=24 || request->receive_count!=1 || request->receive_size<24)
  boundary("nv-tpc-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || 24>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || 24>target.size-(request->receive_address-target.base))
  boundary("nv-tpc-memory",&state,state.pc);
 uint8_t result[24];memcpy(result,(void*)(uintptr_t)request->send_address,sizeof(result));
 if(!ipc_u32(result)) {ipc_reply_u32(buffer,256,4,request->token);return;}
 /* Switch returns two inline masks; the other three input words are opaque.
  * Our single GPC has two TPCs. No second GPC exists. */
 ipc_put32(result+16,(1u<<gpu_model.tpc_per_gpc)-1);ipc_put32(result+20,0);
 memcpy((void*)(uintptr_t)request->receive_address,result,sizeof(result));
 ipc_reply_u32(buffer,256,0,request->token);
}
