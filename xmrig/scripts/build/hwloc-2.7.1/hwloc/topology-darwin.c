/*
 * Copyright © 2009 CNRS
 * Copyright © 2009-2021 Inria.  All rights reserved.
 * Copyright © 2009-2013 Université Bordeaux
 * Copyright © 2009-2011 Cisco Systems, Inc.  All rights reserved.
 * See COPYING in top-level directory.
 */

/* Detect topology change: registering for power management changes and check
 * if for example hw.activecpu changed */

/* Apparently, Darwin people do not _want_ to provide binding functions.  */

#include "private/autogen/config.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <inttypes.h>

#include "hwloc.h"
#include "private/private.h"
#include "private/debug.h"

#if (defined HWLOC_HAVE_DARWIN_FOUNDATION) && (defined HWLOC_HAVE_DARWIN_IOKIT)

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#define DT_PLANE "IODeviceTree"

struct hwloc_darwin_cpukinds {
  struct hwloc_darwin_cpukind {
    hwloc_bitmap_t cpuset;
#define HWLOC_DARWIN_COMPATIBLE_MAX 128
    char *compatible;
  } P, E;
};

static int hwloc__look_darwin_cpukinds(struct hwloc_darwin_cpukinds *kinds)
{
  io_registry_entry_t cpus_root;
  io_iterator_t cpus_iter;
  io_registry_entry_t cpus_child;
  kern_return_t kret;

  hwloc_debug("\nLooking at cpukinds under " DT_PLANE ":/cpus ...\n");

  cpus_root = IORegistryEntryFromPath(kIOMasterPortDefault, DT_PLANE ":/cpus");
  if (!cpus_root) {
    fprintf(stderr, "hwloc/darwin/cpukinds: failed to find " DT_PLANE ":/cpus\n");
    return -1;
  }

  kret = IORegistryEntryGetChildIterator(cpus_root, DT_PLANE, &cpus_iter);
  if (kret != KERN_SUCCESS) {
    if (!hwloc_hide_errors())
      fprintf(stderr, "hwloc/darwin/cpukinds: failed to create iterator\n");
    IOObjectRelease(cpus_root);
    return -1;
  }

  while ((cpus_child = IOIteratorNext(cpus_iter)) != 0) {
    CFTypeRef ref;
    unsigned logical_cpu_id;
    char cluster_type;
    char compatible[HWLOC_DARWIN_COMPATIBLE_MAX+2]; /* room for two \0 at the end */

#ifdef HWLOC_DEBUG
    {
      /* get the name */
      io_name_t name;
      kret = IORegistryEntryGetNameInPlane(cpus_child, DT_PLANE, name);
      if (kret != KERN_SUCCESS) {
        hwloc_debug("failed to find cpu name\n");
      } else {
        hwloc_debug("looking at cpu `%s'\n", name);
      }
    }
#endif

    /* get logical-cpu-id */
    ref = IORegistryEntrySearchCFProperty(cpus_child, DT_PLANE, CFSTR("logical-cpu-id"), kCFAllocatorDefault, kNilOptions);
    if (!ref) {
      /* this may happen on old/x86 systems that aren't hybrid, don't warn */
      hwloc_debug("failed to find logical-cpu-id\n");
      continue;
    }
    if (CFGetTypeID(ref) != CFNumberGetTypeID()) {
      if (!hwloc_hide_errors())
        fprintf(stderr, "hwloc/darwin/cpukinds: unexpected `logical-cpu-id' CF type %s\n",
                CFStringGetCStringPtr(CFCopyTypeIDDescription(CFGetTypeID(ref)), kCFStringEncodingUTF8));
      CFRelease(ref);
      continue;
    }
    {
      long long lld_value;
      if (!CFNumberGetValue(ref, kCFNumberLongLongType, &lld_value)) {
        if (!hwloc_hide_errors())
          fprintf(stderr, "hwloc/darwin/cpukinds: failed to get logical-cpu-id\n");
        CFRelease(ref);
        continue;
      }
      hwloc_debug("got logical-cpu-id %lld\n", lld_value);
      logical_cpu_id = lld_value;
    }
    CFRelease(ref);

#ifdef HWLOC_DEBUG
    /* get logical-cluster-id */
    ref = IORegistryEntrySearchCFProperty(cpus_child, DT_PLANE, CFSTR("logical-cluster-id"), kCFAllocatorDefault, kNilOptions);
    if (!ref) {
      hwloc_debug("failed to find logical-cluster-id\n");
      continue;
    }
    if (CFGetTypeID(ref) != CFNumberGetTypeID()) {
      hwloc_debug("unexpected `logical-cluster-id' CF type is %s\n",
                  CFStringGetCStringPtr(CFCopyTypeIDDescription(CFGetTypeID(ref)), kCFStringEncodingUTF8));
      CFRelease(ref);
      continue;
    }
    {
      long long lld_value;
      if (!CFNumberGetValue(ref, kCFNumberLongLongType, &lld_value)) {
        hwloc_debug("failed to get logical-cluster-id\n");
        CFRelease(ref);
        continue;
      }
      hwloc_debug("got logical-cluster-id %lld\n", lld_value);
    }
    CFRelease(ref);
#endif

    /* get cluster-type */
    ref = IORegistryEntrySearchCFProperty(cpus_child, DT_PLANE, CFSTR("cluster-type"), kCFAllocatorDefault, kNilOptions);
    if (!ref) {
      if (!hwloc_hide_errors())
        fprintf(stderr, "hwloc/darwin/cpukinds: failed to find cluster-type\n");
      continue;
    }
    if (CFGetTypeID(ref) != CFDataGetTypeID()) {
      if (!hwloc_hide_errors())
        fprintf(stderr, "hwloc/darwin/cpukinds: unexpected `cluster-type' CF type %s\n",
                CFStringGetCStringPtr(CFCopyTypeIDDescription(CFGetTypeID(ref)), kCFStringEncodingUTF8));
      CFRelease(ref);
      continue;
    }
    if (CFDataGetLength(ref) < 2) {
      if (!hwloc_hide_errors())
        fprintf(stderr, "hwloc/darwin/cpukinds: only got %ld bytes from cluster-type data\n",
                CFDataGetLength(ref));
      CFRelease(ref);
      continue;
    }
    {
      UInt8 u8_values[2];
      CFDataGetBytes(ref, CFRangeMake(0, 2), u8_values);
      if (u8_values[1] == 0) {
        hwloc_debug("got cluster-type %c\n", u8_values[0]);
        cluster_type = u8_values[0];
      } else {
        if (!hwloc_hide_errors())
          fprintf(stderr, "hwloc/darwin/cpukinds: got more than one character in cluster-type data %c%c...\n",
                  u8_values[0], u8_values[1]);
        CFRelease(ref);
        continue;
      }
    }
    CFRelease(ref);

    /* get compatible */
    ref = IORegistryEntrySearchCFProperty(cpus_child, DT_PLANE, CFSTR("compatible"), kCFAllocatorDefault, kNilOptions);
    if (!ref) {
      if (!hwloc_hide_errors())
        fprintf(stderr, "hwloc/darwin/cpukinds: failed to find compatible\n");
      continue;
    }
    if (CFGetTypeID(ref) != CFDataGetTypeID()) {
      if (!hwloc_hide_errors())
        fprintf(stderr, "hwloc/darwin/cpukinds: unexpected `compatible' CF type %s\n",
                CFStringGetCStringPtr(CFCopyTypeIDDescription(CFGetTypeID(ref)), kCFStringEncodingUTF8));
      CFRelease(ref);
      continue;
    }
    {
      unsigned i, length;
      length = CFDataGetLength(ref);
      if (length > HWLOC_DARWIN_COMPATIBLE_MAX)
        length = HWLOC_DARWIN_COMPATIBLE_MAX;
      CFDataGetBytes(ref, CFRangeMake(0, length), (UInt8*) compatible);
      compatible[length] = 0;
      compatible[length+1] = 0;
      for(i=0; i<length; i++)
        if (!compatible[i] && compatible[i+1])
          compatible[i] = ';';
      if (!compatible[0]) {
        if (!hwloc_hide_errors())
          fprintf(stderr, "hwloc/darwin/cpukinds: compatible is empty\n");
        CFRelease(ref);
        continue;
      }
      hwloc_debug("got compatible %s\n", compatible);
      CFRelease(ref);
    }

    IOObjectRelease(cpus_child);

    /*
     * cluster types: https://developer.apple.com/news/?id=vk3m204o
     * E=Efficiency, P=Performance
     */
    if (cluster_type == 'E') {
      hwloc_bitmap_set(kinds->E.cpuset, logical_cpu_id);
      if (!kinds->E.compatible)
        kinds->E.compatible = strdup(compatible);
      else if (strcmp(kinds->E.compatible, compatible))
        fprintf(stderr, "got a different compatible string inside same cluster\n");

    } else if (cluster_type == 'P') {
      hwloc_bitmap_set(kinds->P.cpuset, logical_cpu_id);
      if (!kinds->P.compatible)
        kinds->P.compatible = strdup(compatible);
      else if (strcmp(kinds->P.compatible, compatible))
        fprintf(stderr, "got a different compatible string inside same cluster\n");

    } else {
      if (!hwloc_hide_errors())
        fprintf(stderr, "hwloc/darwin/cpukinds: unrecognized cluster type %c compatible %s\n",
                cluster_type, compatible);
    }
  }
  IOObjectRelease(cpus_iter);
  IOObjectRelease(cpus_root);

  hwloc_debug("\n");
  return 0;
}

static int hwloc_look_darwin_cpukinds(struct hwloc_topology *topology)
{
    struct hwloc_darwin_cpukinds kinds;

    kinds.P.cpuset = hwloc_bitmap_alloc();
    kinds.P.compatible = NULL;
    kinds.E.cpuset = hwloc_bitmap_alloc();
    kinds.E.compatible = NULL;

    if (!kinds.P.cpuset || !kinds.E.cpuset)
      goto out_with_kinds;

    hwloc__look_darwin_cpukinds(&kinds);

    /* register the cpukind for "P=performance" cores */
    if (!hwloc_bitmap_iszero(kinds.P.cpuset)) {
      struct hwloc_info_s infoattr;
      unsigned nr_info = 0;
      hwloc_debug_1arg_bitmap("building `P' cpukind with compatible `%s' and cpuset %s\n",
                              kinds.P.compatible, kinds.P.cpuset);
      if (kinds.P.compatible) {
        infoattr.name = (char *) "DarwinCompatible";
        infoattr.value = kinds.P.compatible;
        nr_info = 1;
      }
      hwloc_internal_cpukinds_register(topology, kinds.P.cpuset, 1 /* P=performance */, &infoattr, nr_info, 0);
      /* the cpuset is given to the callee */
      topology->support.discovery->cpukind_efficiency = 1;
    } else {
      hwloc_bitmap_free(kinds.P.cpuset);
    }
    free(kinds.P.compatible);

    /* register the cpukind for "E=efficiency" cores */
    if (!hwloc_bitmap_iszero(kinds.E.cpuset)) {
      struct hwloc_info_s infoattr;
      unsigned nr_info = 0;
      hwloc_debug_1arg_bitmap("building `E' cpukind with compatible `%s' and cpuset %s\n",
                              kinds.E.compatible, kinds.E.cpuset);
      if (kinds.E.compatible) {
        infoattr.name = (char *) "DarwinCompatible";
        infoattr.value = kinds.E.compatible;
        nr_info = 1;
      }
      hwloc_internal_cpukinds_register(topology, kinds.E.cpuset, 0 /* E=efficiency */, &infoattr, nr_info, 0);
      /* the cpuset is given to the callee */
      topology->support.discovery->cpukind_efficiency = 1;
    } else {
      hwloc_bitmap_free(kinds.E.cpuset);
    }
    free(kinds.E.compatible);

    hwloc_debug("\n");
    return 0;

 out_with_kinds:
    hwloc_bitmap_free(kinds.P.cpuset);
    free(kinds.P.compatible);
    hwloc_bitmap_free(kinds.E.cpuset);
    free(kinds.E.compatible);
    return -1;
}

#else /* HWLOC_HAVE_DARWIN_FOUNDATION && HWLOC_HAVE_DARWIN_IOKIT */
static int hwloc_look_darwin_cpukinds(struct hwloc_topology *topology __hwloc_attribute_unused)
{
  return 0;
}
#endif /* !HWLOC_HAVE_DARWIN_FOUNDATION || !HWLOC_HAVE_DARWIN_IOKIT */

static int
hwloc_look_darwin(struct hwloc_backend *backend, struct hwloc_disc_status *dstatus)
{
  /*
   * This backend uses the underlying OS.
   * However we don't enforce topology->is_thissystem so that
   * we may still force use this backend when debugging with !thissystem.
   */

  struct hwloc_topology *topology = backend->topology;
  int64_t _nprocs;
  unsigned nprocs;
  int64_t _npackages;
  unsigned i, j, cpu;
  struct hwloc_obj *obj;
  size_t size;
  int64_t l1dcachesize, l1icachesize;
  int64_t cacheways[2];
  int64_t l2cachesize;
  int64_t l3cachesize;
  int64_t cachelinesize;
  int64_t memsize;
  int64_t _tmp;
  char cpumodel[64];
  char cpuvendor[64];
  char cpufamilynumber[20], cpumodelnumber[20], cpustepping[20];
  int gotnuma = 0;
  int gotnumamemory = 0;

  assert(dstatus->phase == HWLOC_DISC_PHASE_CPU);

  if (topology->levels[0][0]->cpuset)
    /* somebody discovered things */
    return -1;

  hwloc_alloc_root_sets(topology->levels[0][0]);

  /* Don't use hwloc_fallback_nbprocessors() because it would return online cpus only,
   * while we need all cpus when computing logical_per_package, etc below.
   * We don't know which CPUs are offline, but Darwin doesn't support binding anyway.
   *
   * TODO: try hw.logicalcpu_max
   */

  if (hwloc_get_sysctlbyname("hw.logicalcpu", &_nprocs) || _nprocs <= 0)
    /* fallback to deprecated way */
    if (hwloc_get_sysctlbyname("hw.ncpu", &_nprocs) || _nprocs <= 0)
      return -1;

  nprocs = _nprocs;
  topology->support.discovery->pu = 1;

  hwloc_debug("%u procs\n", nprocs);

  size = sizeof(cpuvendor);
  if (sysctlbyname("machdep.cpu.vendor", cpuvendor, &size, NULL, 0))
    cpuvendor[0] = '\0';

  size = sizeof(cpumodel);
  if (sysctlbyname("machdep.cpu.brand_string", cpumodel, &size, NULL, 0))
    cpumodel[0] = '\0';

  if (hwloc_get_sysctlbyname("machdep.cpu.family", &_tmp))
    cpufamilynumber[0] = '\0';
  else
    snprintf(cpufamilynumber, sizeof(cpufamilynumber), "%lld", (long long) _tmp);
  if (hwloc_get_sysctlbyname("machdep.cpu.model", &_tmp))
    cpumodelnumber[0] = '\0';
  else
    snprintf(cpumodelnumber, sizeof(cpumodelnumber), "%lld", (long long) _tmp);
  /* .extfamily and .extmodel are already added to .family and .model */
  if (hwloc_get_sysctlbyname("machdep.cpu.stepping", &_tmp))
    cpustepping[0] = '\0';
  else
    snprintf(cpustepping, sizeof(cpustepping), "%lld", (long long) _tmp);

  if (!hwloc_get_sysctlbyname("hw.packages", &_npackages) && _npackages > 0) {
    unsigned npackages = _npackages;
    int64_t _cores_per_package;
    unsigned cores_per_package;
    int64_t _logical_per_package;
    unsigned logical_per_package;

    hwloc_debug("%u packages\n", npackages);

    if (!hwloc_get_sysctlbyname("machdep.cpu.thread_count", &_logical_per_package) && _logical_per_package > 0)
      /* official/modern way */
      logical_per_package = _logical_per_package;
    else if (!hwloc_get_sysctlbyname("machdep.cpu.logical_per_package", &_logical_per_package) && _logical_per_package > 0)
      /* old way, gives the max supported by this "kind" of processor,
       * can be larger than the actual number for this model.
       */
      logical_per_package = _logical_per_package;
    else
      /* Assume the trivia.  */
      logical_per_package = nprocs / npackages;

    hwloc_debug("%u threads per package\n", logical_per_package);

    if (nprocs == npackages * logical_per_package
	&& hwloc_filter_check_keep_object_type(topology, HWLOC_OBJ_PACKAGE))
      for (i = 0; i < npackages; i++) {
        obj = hwloc_alloc_setup_object(topology, HWLOC_OBJ_PACKAGE, i);
        obj->cpuset = hwloc_bitmap_alloc();
        for (cpu = i*logical_per_package; cpu < (i+1)*logical_per_package; cpu++)
          hwloc_bitmap_set(obj->cpuset, cpu);

        hwloc_debug_1arg_bitmap("package %u has cpuset %s\n",
                   i, obj->cpuset);

        if (cpuvendor[0] != '\0')
          hwloc_obj_add_info(obj, "CPUVendor", cpuvendor);
        if (cpumodel[0] != '\0')
          hwloc_obj_add_info(obj, "CPUModel", cpumodel);
        if (cpufamilynumber[0] != '\0')
          hwloc_obj_add_info(obj, "CPUFamilyNumber", cpufamilynumber);
        if (cpumodelnumber[0] != '\0')
          hwloc_obj_add_info(obj, "CPUModelNumber", cpumodelnumber);
        if (cpustepping[0] != '\0')
          hwloc_obj_add_info(obj, "CPUStepping", cpustepping);

        hwloc__insert_object_by_cpuset(topology, NULL, obj, "darwin:package");
      }
    else {
      if (cpuvendor[0] != '\0')
        hwloc_obj_add_info(topology->levels[0][0], "CPUVendor", cpuvendor);
      if (cpumodel[0] != '\0')
        hwloc_obj_add_info(topology->levels[0][0], "CPUModel", cpumodel);
      if (cpufamilynumber[0] != '\0')
        hwloc_obj_add_info(topology->levels[0][0], "CPUFamilyNumber", cpufamilynumber);
      if (cpumodelnumber[0] != '\0')
        hwloc_obj_add_info(topology->levels[0][0], "CPUModelNumber", cpumodelnumber);
      if (cpustepping[0] != '\0')
        hwloc_obj_add_info(topology->levels[0][0], "CPUStepping", cpustepping);
    }

    if (!hwloc_get_sysctlbyname("machdep.cpu.core_count", &_cores_per_package) && _cores_per_package > 0)
      /* official/modern way */
      cores_per_package = _cores_per_package;
    else if (!hwloc_get_sysctlbyname("machdep.cpu.cores_per_package", &_cores_per_package) && _cores_per_package > 0)
      /* old way, gives the max supported by this "kind" of processor,
       * can be larger than the actual number for this model.
       */
      cores_per_package = _cores_per_package;
    else
      /* no idea */
      cores_per_package = 0;

    if (cores_per_package > 0
	&& hwloc_filter_check_keep_object_type(topology, HWLOC_OBJ_CORE)) {
      hwloc_debug("%u cores per package\n", cores_per_package);

      if (!(logical_per_package % cores_per_package))
        for (i = 0; i < npackages * cores_per_package; i++) {
          obj = hwloc_alloc_setup_object(topology, HWLOC_OBJ_CORE, i);
          obj->cpuset = hwloc_bitmap_alloc();
          for (cpu = i*(logical_per_package/cores_per_package);
               cpu < (i+1)*(logical_per_package/cores_per_package);
               cpu++)
            hwloc_bitmap_set(obj->cpuset, cpu);

          hwloc_debug_1arg_bitmap("core %u has cpuset %s\n",
                     i, obj->cpuset);
          hwloc__insert_object_by_cpuset(topology, NULL, obj, "darwin:core");
        }
    }
  } else {
    if (cpuvendor[0] != '\0')
      hwloc_obj_add_info(topology->levels[0][0], "CPUVendor", cpuvendor);
    if (cpumodel[0] != '\0')
      hwloc_obj_add_info(topology->levels[0][0], "CPUModel", cpumodel);
    if (cpufamilynumber[0] != '\0')
      hwloc_obj_add_info(topology->levels[0][0], "CPUFamilyNumber", cpufamilynumber);
    if (cpumodelnumber[0] != '\0')
      hwloc_obj_add_info(topology->levels[0][0], "CPUModelNumber", cpumodelnumber);
    if (cpustepping[0] != '\0')
      hwloc_obj_add_info(topology->levels[0][0], "CPUStepping", cpustepping);
  }

  if (hwloc_get_sysctlbyname("hw.l1dcachesize", &l1dcachesize))
    l1dcachesize = 0;

  if (hwloc_get_sysctlbyname("hw.l1icachesize", &l1icachesize))
    l1icachesize = 0;

  if (hwloc_get_sysctlbyname("hw.l2cachesize", &l2cachesize))
    l2cachesize = 0;

  if (hwloc_get_sysctlbyname("hw.l3cachesize", &l3cachesize))
    l3cachesize = 0;

  if (hwloc_get_sysctlbyname("machdep.cpu.cache.L1_associativity", &cacheways[0]))
    cacheways[0] = 0;
  else if (cacheways[0] == 0xff)
    cacheways[0] = -1;

  if (hwloc_get_sysctlbyname("machdep.cpu.cache.L2_associativity", &cacheways[1]))
    cacheways[1] = 0;
  else if (cacheways[1] == 0xff)
    cacheways[1] = -1;

  if (hwloc_get_sysctlbyname("hw.cachelinesize", &cachelinesize))
    cachelinesize = 0;

  if (hwloc_get_sysctlbyname("hw.memsize", &memsize))
    memsize = 0;

  if (!sysctlbyname("hw.cacheconfig", NULL, &size, NULL, 0)) {
    unsigned n = size / sizeof(uint32_t);
    uint64_t *cacheconfig;
    uint64_t *cachesize;
    uint32_t *cacheconfig32;

    cacheconfig = malloc(n * sizeof(*cacheconfig));
    cachesize = malloc(n * sizeof(*cachesize));
    cacheconfig32 = malloc(n * sizeof(*cacheconfig32));

    if (cacheconfig && cachesize && cacheconfig32
	&& (!sysctlbyname("hw.cacheconfig", cacheconfig, &size, NULL, 0))) {
      /* Yeech. Darwin seemingly has changed from 32bit to 64bit integers for
       * cacheconfig, with apparently no way for detection. Assume the machine
       * won't have more than 4 billion cpus */
      if (cacheconfig[0] > 0xFFFFFFFFUL) {
        memcpy(cacheconfig32, cacheconfig, size);
        for (i = 0 ; i < size / sizeof(uint32_t); i++)
          cacheconfig[i] = cacheconfig32[i];
      }

      memset(cachesize, 0, sizeof(uint64_t) * n);
      size = sizeof(uint64_t) * n;
      if (sysctlbyname("hw.cachesize", cachesize, &size, NULL, 0)) {
        if (n > 0)
          cachesize[0] = memsize;
        if (n > 1)
          cachesize[1] = l1dcachesize;
        if (n > 2)
          cachesize[2] = l2cachesize;
        if (n > 3)
          cachesize[3] = l3cachesize;
      }

      hwloc_debug("%s", "caches");
      for (i = 0; i < n && cacheconfig[i]; i++)
        hwloc_debug(" %"PRIu64"(%"PRIu64"kB)", cacheconfig[i], cachesize[i] / 1024);

      /* Now we know how many caches there are */
      n = i;
      hwloc_debug("\n%u cache levels\n", n - 1);

      /* For each cache level (0 is memory) */
      for (i = 0; i < n; i++) {
        /* cacheconfig tells us how many cpus share it, let's iterate on each cache */
        for (j = 0; j < (nprocs / cacheconfig[i]); j++) {
	  if (!i) {
	    obj = hwloc_alloc_setup_object(topology, HWLOC_OBJ_NUMANODE, j);
            obj->nodeset = hwloc_bitmap_alloc();
            hwloc_bitmap_set(obj->nodeset, j);
	    gotnuma++;
          } else {
	    obj = hwloc_alloc_setup_object(topology, HWLOC_OBJ_L1CACHE+i-1, HWLOC_UNKNOWN_INDEX);
	  }
          obj->cpuset = hwloc_bitmap_alloc();
          hwloc_bitmap_set_range(obj->cpuset, j*cacheconfig[i], (j+1)*cacheconfig[i]-1);

          if (i == 1 && l1icachesize
	      && hwloc_filter_check_keep_object_type(topology, HWLOC_OBJ_L1ICACHE)) {
            /* FIXME assuming that L1i and L1d are shared the same way. Darwin
             * does not yet provide a way to know.  */
            hwloc_obj_t l1i = hwloc_alloc_setup_object(topology, HWLOC_OBJ_L1ICACHE, HWLOC_UNKNOWN_INDEX);
            l1i->cpuset = hwloc_bitmap_dup(obj->cpuset);
            hwloc_debug_1arg_bitmap("L1icache %u has cpuset %s\n",
                j, l1i->cpuset);
            l1i->attr->cache.depth = i;
            l1i->attr->cache.size = l1icachesize;
            l1i->attr->cache.linesize = cachelinesize;
            l1i->attr->cache.associativity = 0;
            l1i->attr->cache.type = HWLOC_OBJ_CACHE_INSTRUCTION;

            hwloc__insert_object_by_cpuset(topology, NULL, l1i, "darwin:l1icache");
          }
          if (i) {
            hwloc_debug_2args_bitmap("L%ucache %u has cpuset %s\n",
                i, j, obj->cpuset);
            obj->attr->cache.depth = i;
            obj->attr->cache.size = cachesize[i];
            obj->attr->cache.linesize = cachelinesize;
            if (i <= sizeof(cacheways) / sizeof(cacheways[0]))
              obj->attr->cache.associativity = cacheways[i-1];
            else
              obj->attr->cache.associativity = 0;
            if (i == 1 && l1icachesize)
              obj->attr->cache.type = HWLOC_OBJ_CACHE_DATA;
            else
              obj->attr->cache.type = HWLOC_OBJ_CACHE_UNIFIED;
          } else {
            hwloc_debug_1arg_bitmap("node %u has cpuset %s\n",
                j, obj->cpuset);
	    if (cachesize[i]) {
	      obj->attr->numanode.local_memory = cachesize[i];
	      gotnumamemory++;
	    }
	    obj->attr->numanode.page_types_len = 2;
	    obj->attr->numanode.page_types = malloc(2*sizeof(*obj->attr->numanode.page_types));
	    memset(obj->attr->numanode.page_types, 0, 2*sizeof(*obj->attr->numanode.page_types));
	    obj->attr->numanode.page_types[0].size = hwloc_getpagesize();
#if HAVE_DECL__SC_LARGE_PAGESIZE
	    obj->attr->numanode.page_types[1].size = sysconf(_SC_LARGE_PAGESIZE);
#endif
          }
          if (hwloc_filter_check_keep_object_type(topology, obj->type))
            hwloc__insert_object_by_cpuset(topology, NULL, obj,
                                           obj->type == HWLOC_OBJ_NUMANODE ? "darwin:numanode" : "darwin:cache");
          else
            hwloc_free_unlinked_object(obj); /* FIXME: don't built at all, just build the cpuset in case l1i needs it */
        }
      }
    }
    free(cacheconfig);
    free(cachesize);
    free(cacheconfig32);
  }

  if (gotnuma)
    topology->support.discovery->numa = 1;
  if (gotnumamemory)
    topology->support.discovery->numa = 1;

  /* add PU objects */
  hwloc_setup_pu_level(topology, nprocs);

  hwloc_look_darwin_cpukinds(topology);

  hwloc_obj_add_info(topology->levels[0][0], "Backend", "Darwin");
  hwloc_add_uname_info(topology, NULL);
  return 0;
}

void
hwloc_set_darwin_hooks(struct hwloc_binding_hooks *hooks __hwloc_attribute_unused,
		       struct hwloc_topology_support *support __hwloc_attribute_unused)
{
}

static struct hwloc_backend *
hwloc_darwin_component_instantiate(struct hwloc_topology *topology,
				   struct hwloc_disc_component *component,
				   unsigned excluded_phases __hwloc_attribute_unused,
				   const void *_data1 __hwloc_attribute_unused,
				   const void *_data2 __hwloc_attribute_unused,
				   const void *_data3 __hwloc_attribute_unused)
{
  struct hwloc_backend *backend;
  backend = hwloc_backend_alloc(topology, component);
  if (!backend)
    return NULL;
  backend->discover = hwloc_look_darwin;
  return backend;
}

static struct hwloc_disc_component hwloc_darwin_disc_component = {
  "darwin",
  HWLOC_DISC_PHASE_CPU,
  HWLOC_DISC_PHASE_GLOBAL,
  hwloc_darwin_component_instantiate,
  50,
  1,
  NULL
};

const struct hwloc_component hwloc_darwin_component = {
  HWLOC_COMPONENT_ABI,
  NULL, NULL,
  HWLOC_COMPONENT_TYPE_DISC,
  0,
  &hwloc_darwin_disc_component
};
