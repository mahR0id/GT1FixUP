#import <Metal/Metal.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <string.h>
#include <stdio.h>
static int count(const uint8_t *p, unsigned long n, const uint8_t *f, size_t l){int c=0;for(unsigned long i=0;i+l<=n;i++) if(p[i]==f[0]&&!memcmp(p+i,f,l)) c++; return c;}
int main(){@autoreleasepool{
 id<MTLDevice> d=MTLCreateSystemDefaultDevice(); printf("device: %s\n", d.name.UTF8String);
 const uint8_t oW[]={0x48,0xB9,0,0,0x30,0x78,0x80,3,3,8}, nW[]={0x48,0xB9,0,0,0x30,0x78,0,1,3,8};
 const uint8_t oT[]={0,0,0x30,0x78,0x80,3,3,8,0,0,0x31,0x78}, nT[]={0,0,0x30,0x78,0,1,3,8,0,0,0x31,0x78};
 const uint8_t oP[]={0x80,3,3,8,0,0,0,0,0,0,0,0x40,0,0,0,0,0,0,0,0x40}, nP[]={0,1,3,8,0,0,0,0,0,0,0,0x40,0,0,0,0,0,0,0,0x40};
 const uint8_t oU[]={0x31,0xC0,0x83,0x79,0x10,0,0x0F,0x95,0xC0,0xC1,0xE0,4,0x83,0xC0,0x10}, nU[]={0x31,0xC0,0x83,0x79,0x10,0,0x90,0x90,0x90,0xC1,0xE0,4,0x83,0xC0,0x10};
 const uint8_t oD[]={0x80,3,3,8,0,0,0,8,0,0,0,8,0,0,0,8}, nD[]={0,1,3,8,0,0,0,8,0,0,0,8,0,0,0,8};
 for(uint32_t i=0;i<_dyld_image_count();i++){ const char*n=_dyld_get_image_name(i);
  if(!strstr(n,"GraphicsMTLDriver")&&!strstr(n,"GraphicsGLDriver")) continue;
  const struct mach_header_64*h=(const struct mach_header_64*)_dyld_get_image_header(i);
  unsigned long sz=0; const uint8_t*t=getsegmentdata(h,"__TEXT",&sz);
  printf("%s\n  __TEXT %lu bytes  writeInit old=%d new=%d | template old=%d new=%d | defaults old=%d new=%d | payloads old=%d new=%d | urbsize old=%d new=%d\n", n, sz,
   count(t,sz,oW,10),count(t,sz,nW,10),count(t,sz,oT,12),count(t,sz,nT,12),count(t,sz,oD,16),count(t,sz,nD,16),count(t,sz,oP,20),count(t,sz,nP,20),count(t,sz,oU,15),count(t,sz,nU,15)); }
}}
