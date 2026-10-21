// Select the Metal device by name substring: GPU=NVIDIA ./test  (default: system default device).
#import <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>
static id<MTLDevice> pickDevice(void) {
  const char *want = getenv("GPU");
  if (want && *want) {
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    for (id<MTLDevice> d in all) if (strcasestr(d.name.UTF8String, want)) return d;
    printf("no Metal device matching '%s'; available:", want);
    for (id<MTLDevice> d in all) printf(" [%s]", d.name.UTF8String);
    printf("\n"); exit(2);
  }
  return MTLCreateSystemDefaultDevice();
}
