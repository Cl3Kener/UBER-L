#ifndef MM_SLAB_H
#define MM_SLAB_H
/*
 * Internal slab definitions
 */

/*
 * State of the slab allocator.
 *
 * This is used to describe the states of the allocator during bootup.
 * Allocators use this to gradually bootstrap themselves. Most allocators
 * have the problem that the structures used for managing slab caches are
 * allocated from slab caches themselves.
 */
enum slab_state {
	DOWN,			/* No slab functionality yet */
	PARTIAL,		/* SLUB: kmem_cache_node available */
	PARTIAL_ARRAYCACHE,	/* SLAB: kmalloc size for arraycache available */
	PARTIAL_L3,		/* SLAB: kmalloc size for l3 struct available */
	UP,			/* Slab caches usable but not all extras yet */
	FULL			/* Everything is working */
};

extern enum slab_state slab_state;

/* The slab cache mutex protects the management structures during changes */
extern struct mutex slab_mutex;

/* The list of all slab caches on the system */
extern struct list_head slab_caches;

/* The slab cache that manages slab cache information */
extern struct kmem_cache *kmem_cache;

unsigned long calculate_alignment(unsigned long flags,
		unsigned long align, unsigned long size);

/* Functions provided by the slab allocators */
extern int __kmem_cache_create(struct kmem_cache *, unsigned long flags);

extern struct kmem_cache *create_kmalloc_cache(const char *name, size_t size,
			unsigned long flags);
extern void create_boot_cache(struct kmem_cache *, const char *name,
			size_t size, unsigned long flags);

#ifdef CONFIG_SLUB
struct kmem_cache *__kmem_cache_alias(const char *name, size_t size,
	size_t align, unsigned long flags, void (*ctor)(void *));
#else
static inline struct kmem_cache *__kmem_cache_alias(const char *name, size_t size,
	size_t align, unsigned long flags, void (*ctor)(void *))
{ return NULL; }
#endif


/* Legal flag mask for kmem_cache_create(), for various configurations */
#define SLAB_CORE_FLAGS (SLAB_HWCACHE_ALIGN | SLAB_CACHE_DMA | SLAB_PANIC | \
			 SLAB_DESTROY_BY_RCU | SLAB_DEBUG_OBJECTS )

#if defined(CONFIG_DEBUG_SLAB)
#define SLAB_DEBUG_FLAGS (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER)
#elif defined(CONFIG_SLUB_DEBUG)
#define SLAB_DEBUG_FLAGS (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER | \
			  SLAB_TRACE | SLAB_DEBUG_FREE)
#else
#define SLAB_DEBUG_FLAGS (0)
#endif

#if defined(CONFIG_SLAB)
#define SLAB_CACHE_FLAGS (SLAB_MEM_SPREAD | SLAB_NOLEAKTRACE | \
			  SLAB_RECLAIM_ACCOUNT | SLAB_TEMPORARY | SLAB_NOTRACK)
#elif defined(CONFIG_SLUB)
#define SLAB_CACHE_FLAGS (SLAB_NOLEAKTRACE | SLAB_RECLAIM_ACCOUNT | \
			  SLAB_TEMPORARY | SLAB_NOTRACK)
#else
#define SLAB_CACHE_FLAGS (0)
#endif

#define CACHE_CREATE_MASK (SLAB_CORE_FLAGS | SLAB_DEBUG_FLAGS | SLAB_CACHE_FLAGS)

int __kmem_cache_shutdown(struct kmem_cache *);

struct seq_file;
struct file;

struct slabinfo {
	unsigned long active_objs;
	unsigned long num_objs;
	unsigned long active_slabs;
	unsigned long num_slabs;
	unsigned long shared_avail;
	unsigned int limit;
	unsigned int batchcount;
	unsigned int shared;
	unsigned int objects_per_slab;
	unsigned int cache_order;
};

void get_slabinfo(struct kmem_cache *s, struct slabinfo *sinfo);
void slabinfo_show_stats(struct seq_file *m, struct kmem_cache *s);
ssize_t slabinfo_write(struct file *file, const char __user *buffer,
		       size_t count, loff_t *ppos);

static inline bool slab_equal_or_root(struct kmem_cache *s,
				      struct kmem_cache *p)
{
	return true;
}

static inline struct kmem_cache *cache_from_obj(struct kmem_cache *s, void *x)
{
	struct kmem_cache *cachep;
	struct page *page;

	if (!unlikely(s->flags & SLAB_DEBUG_FREE))
		return s;

	page = virt_to_head_page(x);
	cachep = page->slab_cache;
	if (slab_equal_or_root(cachep, s))
		return cachep;

	pr_err("%s: Wrong slab cache. %s but object is from %s\n",
		__FUNCTION__, cachep->name, s->name);
	WARN_ON_ONCE(1);
	return s;
}
#endif
