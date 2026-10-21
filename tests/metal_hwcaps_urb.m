#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
static NSString *S=@"#include <metal_stdlib>\nusing namespace metal;\nstruct VO{float4 pos [[position]]; float4 c;};\n"
"vertex VO vs(uint vid [[vertex_id]], constant uint *G [[buffer(0)]]){ uint q=vid/6, c=vid%6, g=G[0]; uint cx=q%g, cy=q/g;\n"
" float2 k[6]={float2(0,0),float2(1,0),float2(0,1),float2(1,0),float2(1,1),float2(0,1)}; float2 p=(float2(cx,cy)+k[c])/float(g)*2.0-1.0;\n"
" VO o; o.pos=float4(p.x,-p.y,0,1); o.c=float4(0,1,0,1); return o;}\nfragment float4 fs(VO i [[stage_in]]){return i.c;}\n";
int main(int argc,char**argv){@autoreleasepool{
 int zero = argc>1 && atoi(argv[1]);
 id<MTLDevice> d=MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q=[d newCommandQueue]; NSError*e=nil;
 id<MTLLibrary> l=[d newLibraryWithSource:S options:nil error:&e]; MTLRenderPipelineDescriptor*pd=[MTLRenderPipelineDescriptor new];
 pd.vertexFunction=[l newFunctionWithName:@"vs"]; pd.fragmentFunction=[l newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
 id<MTLRenderPipelineState> ps0=[d newRenderPipelineStateWithDescriptor:pd error:&e];
 Ivar iv=class_getInstanceVariable(object_getClass(ps0),"mHWCaps"); uint8_t *caps=*(uint8_t**)((uint8_t*)(__bridge void*)ps0+ivar_getOffset(iv));
 printf("mHWCaps %p:",caps); for(int i=0;i<0x40;i+=4) printf("%s%08x",i%16?" ":"\n  ",*(uint32_t*)(caps+i)); printf("\n");
 if(zero){ *(uint32_t*)(caps+0x10)=0; printf("zeroed caps+0x10\n"); }
 pd.label=@"fresh"; id<MTLRenderPipelineState> ps=[d newRenderPipelineStateWithDescriptor:pd error:&e];
 uint8_t *o=(uint8_t*)(__bridge void*)ps; size_t n=malloc_size(o);
 for(size_t i=0;i+8<=n;i+=4) if(*(uint32_t*)(o+i)==0x78300000) printf("  URB_VS entries=%u\n", *(uint32_t*)(o+i+4)&0xffff);
 const uint W=512, G=128; MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:W mipmapped:NO];
 td.usage=MTLTextureUsageRenderTarget; td.storageMode=MTLStorageModeManaged; id<MTLTexture> t=[d newTextureWithDescriptor:td]; static uint8_t px[512*512*4];
 MTLRenderPassDescriptor*rp=[MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture=t; rp.colorAttachments[0].loadAction=MTLLoadActionClear;
 rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1); rp.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLRenderCommandEncoder> en=[cb renderCommandEncoderWithDescriptor:rp]; [en setRenderPipelineState:ps];
 uint a[1]={G}; [en setVertexBytes:a length:4 atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:G*G*6];
 [en endEncoding]; id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder]; [b synchronizeResource:t]; [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
 [t getBytes:px bytesPerRow:W*4 fromRegion:MTLRegionMake2D(0,0,W,W) mipmapLevel:0];
 long tri=0; for(uint qy=0;qy<G;qy++)for(uint qx=0;qx<G;qx++){ if(px[((qy*4)*W+qx*4)*4+1]>200) tri++; if(px[((qy*4+3)*W+qx*4+3)*4+1]>200) tri++; }
 printf("triangles drawn: %.1f%%\n",100.0*tri/(2.0*G*G));
}}
