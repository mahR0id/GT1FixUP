// Sweep: grid size G (quads per draw = G*G) and draws-per-pass (split into chunks of C quads).
#import <Metal/Metal.h>
#include "pick_device.h"
#include <stdio.h>
static NSString *S=@"#include <metal_stdlib>\nusing namespace metal;\nstruct VO{float4 pos [[position]]; float4 c;};\n"
"vertex VO vs(uint vid [[vertex_id]], constant uint *G [[buffer(0)]]){ uint q=vid/6+G[1], c=vid%6, g=G[0]; uint cx=q%g, cy=q/g;\n"
" float2 k[6]={float2(0,0),float2(1,0),float2(0,1),float2(1,0),float2(1,1),float2(0,1)}; float2 p=(float2(cx,cy)+k[c])/float(g)*2.0-1.0;\n"
" VO o; o.pos=float4(p.x,-p.y,0,1); o.c=float4(0,1,0,1); return o;}\nfragment float4 fs(VO i [[stage_in]]){return i.c;}\n";
int main(){@autoreleasepool{
 id<MTLDevice> d=pickDevice(); id<MTLCommandQueue> q=[d newCommandQueue]; NSError*e=nil;
 id<MTLLibrary> l=[d newLibraryWithSource:S options:nil error:&e]; MTLRenderPipelineDescriptor*pd=[MTLRenderPipelineDescriptor new];
 pd.vertexFunction=[l newFunctionWithName:@"vs"]; pd.fragmentFunction=[l newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
 id<MTLRenderPipelineState> ps=[d newRenderPipelineStateWithDescriptor:pd error:&e];
 const uint W=512; MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:W mipmapped:NO];
 td.usage=MTLTextureUsageRenderTarget; td.storageMode=MTLStorageModeManaged; id<MTLTexture> t=[d newTextureWithDescriptor:td];
 static uint8_t px[512*512*4];
 uint Gs[]={1,2,4,8,16,32,64,128}; uint Cs[]={0,1,8,64};
 printf("%-6s %-8s","G","quads"); for(int c=0;c<4;c++) printf(Cs[c]?"  %5u/draw":"  one-draw ",Cs[c]); printf("   (%% quads fully drawn)\n");
 for(int gi=0;gi<8;gi++){ uint G=Gs[gi]; printf("%-6u %-8u",G,G*G);
  for(int ci=0;ci<4;ci++){ uint C=Cs[ci]?Cs[ci]:G*G; if(C>G*G){printf("  %10s","-");continue;}
   MTLRenderPassDescriptor*rp=[MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture=t; rp.colorAttachments[0].loadAction=MTLLoadActionClear;
   rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1); rp.colorAttachments[0].storeAction=MTLStoreActionStore;
   id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLRenderCommandEncoder> en=[cb renderCommandEncoderWithDescriptor:rp]; [en setRenderPipelineState:ps];
   for(uint s=0;s<G*G;s+=C){ uint a[2]={G,s}; [en setVertexBytes:a length:8 atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:MIN(C,G*G-s)*6]; }
   [en endEncoding]; id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder]; [b synchronizeResource:t]; [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
   [t getBytes:px bytesPerRow:W*4 fromRegion:MTLRegionMake2D(0,0,W,W) mipmapLevel:0];
   uint cell=W/G, ok=0; for(uint qy=0;qy<G;qy++)for(uint qx=0;qx<G;qx++){ int full=1;
     uint ys[2]={qy*cell+cell/4, qy*cell+3*cell/4}, xs[2]={qx*cell+cell/4, qx*cell+3*cell/4};  // one sample in each triangle half
     for(int i=0;i<2;i++) if(px[(ys[i]*W+xs[i])*4+1]<200) full=0; ok+=full; }
   printf("  %9.1f%%",100.0*ok/(G*G)); }
  printf("\n"); }
}}
