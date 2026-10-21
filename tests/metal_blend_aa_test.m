// Checks the GPU paths text edges depend on: alpha blending, linear texture filtering
// (scaled glyph textures) and 4x MSAA resolve. Prints checksums/values to compare across machines.
#import <Metal/Metal.h>
#include "pick_device.h"
#include <stdio.h>
static NSString *S=@"#include <metal_stdlib>\nusing namespace metal;\n"
"struct VO{float4 pos [[position]]; float2 uv;};\n"
"vertex VO vs(uint v [[vertex_id]], constant float4 *q [[buffer(0)]]){ float2 k[6]={float2(0,0),float2(1,0),float2(0,1),float2(1,0),float2(1,1),float2(0,1)};\n"
" VO o; float2 p=mix(q[0].xy,q[0].zw,k[v]); o.pos=float4(p,0,1); o.uv=float2(k[v].x,1-k[v].y); return o;}\n"
"fragment float4 fsTex(VO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]){ return t.sample(s,i.uv); }\n"
"fragment float4 fsSolid(VO i [[stage_in]]){ return float4(1,1,1,1); }\n";
static id<MTLDevice> d; static id<MTLCommandQueue> q;
static id<MTLRenderPipelineState> mkpipe(id<MTLLibrary> l, NSString *fs, int blend, int samples){
  MTLRenderPipelineDescriptor *p=[MTLRenderPipelineDescriptor new]; p.vertexFunction=[l newFunctionWithName:@"vs"]; p.fragmentFunction=[l newFunctionWithName:fs];
  p.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm; p.rasterSampleCount=samples;
  if(blend){ p.colorAttachments[0].blendingEnabled=YES; p.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
    p.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha; p.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;
    p.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha; }
  NSError *e; id<MTLRenderPipelineState> s=[d newRenderPipelineStateWithDescriptor:p error:&e]; if(!s) printf("pipeline: %s\n",e.description.UTF8String); return s; }
static id<MTLTexture> tex(int w,int h,int samples,int rt){ MTLTextureDescriptor *t=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
  if(samples>1){ t.textureType=MTLTextureType2DMultisample; t.sampleCount=samples; t.storageMode=MTLStorageModePrivate; } else t.storageMode=MTLStorageModeManaged;
  t.usage=rt?MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead:MTLTextureUsageShaderRead; return [d newTextureWithDescriptor:t]; }
static int dumpN; static unsigned sum(id<MTLTexture> t, uint8_t *px){ [t getBytes:px bytesPerRow:t.width*4 fromRegion:MTLRegionMake2D(0,0,t.width,t.height) mipmapLevel:0];
  if(getenv("DUMP")){ char fn[256]; snprintf(fn,256,"%s.%d",getenv("DUMP"),dumpN++); FILE*f=fopen(fn,"wb"); fwrite(px,1,t.width*t.height*4,f); fclose(f); }
  unsigned h=2166136261u; for(NSUInteger i=0;i<t.width*t.height*4;i++){h^=px[i];h*=16777619u;} return h; }
int main(){@autoreleasepool{
  d=pickDevice(); q=[d newCommandQueue]; printf("device: %s\n",d.name.UTF8String); NSError *e;
  id<MTLLibrary> l=[d newLibraryWithSource:S options:nil error:&e];
  static uint8_t px[256*256*4];
  // source "glyph": 8x8 texture with alpha ramp (like an antialiased edge), white premultiplied-free
  id<MTLTexture> src=tex(8,8,1,0); uint8_t s[8*8*4]; for(int y=0;y<8;y++)for(int x=0;x<8;x++){uint8_t a=(uint8_t)(x*255/7); uint8_t*p=&s[(y*8+x)*4]; p[0]=p[1]=p[2]=255; p[3]=a;}
  [src replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:s bytesPerRow:32];
  MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new]; sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterLinear; id<MTLSamplerState> lin=[d newSamplerStateWithDescriptor:sd];
  // 1) blend scaled glyph texture (8x8 -> 64x64, linear) over dark grey background
  id<MTLTexture> rt=tex(64,64,1,1); id<MTLRenderPipelineState> pb=mkpipe(l,@"fsTex",1,1);
  MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture=rt; rp.colorAttachments[0].loadAction=MTLLoadActionClear;
  rp.colorAttachments[0].clearColor=MTLClearColorMake(0.2,0.2,0.2,1); rp.colorAttachments[0].storeAction=MTLStoreActionStore;
  id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLRenderCommandEncoder> en=[cb renderCommandEncoderWithDescriptor:rp];
  float quad[4]={-1,-1,1,1}; [en setRenderPipelineState:pb]; [en setVertexBytes:quad length:16 atIndex:0]; [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:lin atIndex:0];
  [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6]; [en endEncoding];
  id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder]; [b synchronizeResource:rt]; [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
  unsigned h1=sum(rt,px); printf("blend+linear row32 (B of x=0,8,..,56):"); for(int x=0;x<64;x+=8) printf(" %3d",px[(32*64+x)*4]); printf("   checksum %08x\n",h1);
  // 2) 4x MSAA: a slanted triangle edge, resolved
  id<MTLTexture> ms=tex(64,64,4,1), res=tex(64,64,1,1); id<MTLRenderPipelineState> pm=mkpipe(l,@"fsSolid",0,4);
  rp=[MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture=ms; rp.colorAttachments[0].resolveTexture=res; rp.colorAttachments[0].loadAction=MTLLoadActionClear;
  rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1); rp.colorAttachments[0].storeAction=MTLStoreActionMultisampleResolve;
  cb=[q commandBuffer]; en=[cb renderCommandEncoderWithDescriptor:rp]; float tri[4]={-1,-1,1,0.3f}; [en setRenderPipelineState:pm]; [en setVertexBytes:tri length:16 atIndex:0];
  [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
  b=[cb blitCommandEncoder]; [b synchronizeResource:res]; [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
  unsigned h2=sum(res,px); int partial=0; for(int i=0;i<64*64;i++){int v=px[i*4]; if(v>0&&v<255) partial++;}
  printf("4x MSAA resolve: %d edge pixels with partial coverage   checksum %08x\n",partial,h2);
}}
