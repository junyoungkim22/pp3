#include "fte.h"

uint8_t *allocate_frame(void *vaddr, enum palloc_flags flag, bool writable)
{
	if(lock_held_by_current_thread(&frame_lock))
	{
		printf("lock fail in allocate frame\n");
		lock_release(&frame_lock);
		shutdown_power_off();
	}
	lock_acquire(&frame_lock);
	//printf("Allocate start\n");
	uint8_t *kpage;
	bool success = false;
	struct fte *new_fte;
	struct fte *evict_fte;
	struct sup_pte *new_sup_pte;

	kpage = palloc_get_page(PAL_USER | flag);
	if(kpage == NULL){
		evict_fte = fte_to_evict();
		/*
		printf("evicting vaddr: %p\n", evict_fte->spte->vaddr);
		printf("new vaddr: %p\n", vaddr);
		*/
		if(!evict(evict_fte))
		{
			printf("Eviction failed in allocate frame\n");
			lock_release(&frame_lock);
			//sys_exit(-1);
			shutdown_power_off();
			return NULL;
		}
		kpage = palloc_get_page(PAL_USER | flag);
		if(kpage == NULL)
		{
			printf("Allocation after eviction failed in allocate frame\n");
			lock_release(&frame_lock);
			//sys_exit(-1);
			shutdown_power_off();
			return NULL;
		}
	}
	success = install_page(vaddr, kpage, writable);
	if(!success)
	{
		palloc_free_page(kpage);
		printf("Installation of page failed in allocate page\n");
		lock_release(&frame_lock);
		//sys_exit(-1);
		shutdown_power_off();
		return NULL;
	}

	new_fte = malloc(sizeof (struct fte));
	new_fte->owner = thread_current();
	new_fte->frame = kpage;

	new_sup_pte = malloc(sizeof (struct sup_pte));
	new_sup_pte->vaddr = vaddr;
	new_sup_pte->access_time = timer_ticks();
	new_sup_pte->writable = writable;
	new_sup_pte->flag = flag;
	new_sup_pte->can_evict = true;
	new_sup_pte->is_mmap = false;
	
	new_fte->spte = new_sup_pte;

	hash_insert(&thread_current()->sup_page_table, &new_sup_pte->hash_elem);
	list_push_back(&frame_table, &new_fte->ft_elem);
	
	new_sup_pte->allocated = true;
	//printf("Allocate end\n");
	lock_release(&frame_lock);
	return kpage;
}

bool evict(struct fte *fte_to_evict)
{
	size_t index;
	struct file *mmap_file;
	struct mmap_info *spte_mmap_info;
	//printf("evict start\n");
	if(fte_to_evict->spte->is_mmap && pagedir_is_dirty(thread_current()->pagedir, fte_to_evict->spte->vaddr))
	{
		lock_acquire(&filesys_lock);
		spte_mmap_info = get_mmap_info(fte_to_evict->spte->vaddr);
		mmap_file = spte_mmap_info->mmap_file;
		file_write_at(mmap_file, fte_to_evict->frame, spte_mmap_info->size, spte_mmap_info->file_index);
		lock_release(&filesys_lock);
	}
	else 
	{
		index= bitmap_scan(sector_bitmap, 0, 8, false);
		if(index == BITMAP_ERROR)
		{
			printf("No space in swap!\n");
			return false;
		}
		write_to_block(fte_to_evict->frame, index);
		block_mark(index);
		fte_to_evict->spte->disk_index = index;
	}
	pagedir_clear_page(fte_to_evict->owner->pagedir, fte_to_evict->spte->vaddr);
	palloc_free_page(fte_to_evict->frame);
	list_remove(&fte_to_evict->ft_elem);
	fte_to_evict->spte->allocated = false;
	free(fte_to_evict);
	return true;
}

bool load_mmap(struct sup_pte *spte)
{
	uint8_t *kpage;
	struct fte *evict_fte;
	struct fte *new_fte;
	struct mmap_info *spte_mmap_info;
	struct file *mmap_file;
	size_t read_bytes;
	bool success;

	if(lock_held_by_current_thread(&frame_lock))
	{
		printf("2");
		lock_release(&frame_lock);
		//sys_exit(-1);
		shutdown_power_off();
	}
	lock_acquire(&frame_lock);
	if(spte->allocated)
	{
		lock_release(&frame_lock);
		return true;
	}
	spte_mmap_info = get_mmap_info(spte->vaddr);
	if(spte_mmap_info == NULL)
	{
		lock_release(&frame_lock);
		return false;
	}
	if(spte_mmap_info->size == 0)
	{
		lock_release(&frame_lock);
		sys_exit(-1);
	}
	kpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if(kpage == NULL){
		evict_fte = fte_to_evict();
		/*
		printf("evicting vaddr: %p\n", evict_fte->spte->vaddr);
		printf("new vaddr: %p\n", vaddr);
		*/
		if(!evict(evict_fte))
		{
			printf("Eviction failed\n");
			lock_release(&frame_lock);
			sys_exit(-1);
			return false;
		}
		kpage = palloc_get_page(PAL_USER | PAL_ZERO);
		if(kpage == NULL)
		{
			printf("Allocation after eviction failed\n");
			lock_release(&frame_lock);
			sys_exit(-1);
			return false;
		}
	}
	success = install_page(spte->vaddr, kpage, true);
	if(!success)
	{
		palloc_free_page(kpage);
		printf("Installation of page failed\n");
		lock_release(&frame_lock);
		//sys_exit(-1);
		shutdown_power_off();
		return false;
	}

	lock_acquire(&filesys_lock);
	mmap_file = spte_mmap_info->mmap_file;
	read_bytes = spte_mmap_info->size;
	file_read_at(mmap_file, kpage, read_bytes, spte_mmap_info->file_index);
	memset(kpage + read_bytes, 0, PGSIZE - read_bytes);
	lock_release(&filesys_lock);

	new_fte = malloc(sizeof (struct fte));
	new_fte->owner = thread_current();
	new_fte->frame = kpage;
	new_fte->spte = spte;
	list_push_back(&frame_table, &new_fte->ft_elem);
	spte->allocated = true;	

	lock_release(&frame_lock);
	return true;
}

bool load_sup_pte(struct sup_pte *spte)
{
	uint8_t *kpage;
	struct fte *evict_fte;
	struct fte *new_fte;
	bool success;

	if(lock_held_by_current_thread(&frame_lock))
	{
		printf("3");
		lock_release(&frame_lock);
		//sys_exit(-1);
		shutdown_power_off();
	}
	lock_acquire(&frame_lock);
	//printf("load spte start\n");
	spte->can_evict = false;
	if(spte->allocated)
	{
		lock_release(&frame_lock);
		return true;
	}
	kpage = palloc_get_page(PAL_USER | spte->flag);
	if(kpage == NULL){
		evict_fte = fte_to_evict();
		/*
		printf("evicting vaddr: %p\n", evict_fte->spte->vaddr);
		printf("new vaddr: %p\n", vaddr);
		*/
		if(!evict(evict_fte))
		{
			printf("Eviction failed in load_sup_pte\n");
			lock_release(&frame_lock);
			shutdown_power_off();
			return NULL;
		}
		kpage = palloc_get_page(PAL_USER | spte->flag);
		if(kpage == NULL)
		{
			printf("Allocation after eviction failed in load_sup_pte\n");
			lock_release(&frame_lock);
			shutdown_power_off();
			return NULL;
		}
	}
	/*
	success = install_page(spte->vaddr, kpage, true);
	if(!success)
	{
		palloc_free_page(kpage);
		printf("Installation of page failed\n");
		lock_release(&frame_lock);
		//sys_exit(-1);
		shutdown_power_off();
		return NULL;
	}
	*/

	/*
	evict_fte = fte_to_evict();
	evict(evict_fte);
	kpage = palloc_get_page(PAL_USER | spte->flag);
	if(kpage == NULL)		
	{
		printf("Allocation after eviction failed\n");
		lock_release(&frame_lock);
		return false;
	}
	*/

	read_from_block(kpage, spte->disk_index);
	block_reset(spte->disk_index);

	success = install_page(spte->vaddr, kpage, spte->writable);
	if(!success)
	{
		palloc_free_page(kpage);
		lock_release(&frame_lock);
		return success;
	}

	new_fte = malloc(sizeof (struct fte));
	new_fte->owner = thread_current();
	new_fte->frame = kpage;
	new_fte->spte = spte;
	
	list_push_back(&frame_table, &new_fte->ft_elem);
	//printf("load spte end\n");
	spte->allocated = true;	
	lock_release(&frame_lock);
	return true;
}

struct fte *fte_to_evict()
{
	int n = list_size(&frame_table);
	struct fte *e;
	struct list_elem *it;
	//printf("eviction choose start\n");
	it = list_begin(&frame_table);
	for(int i = 0; i < 2 * (list_size(&frame_table)); i++)
	{
		e = list_entry(it, struct fte, ft_elem);
		if(e->spte->can_evict)
		{
			if(pagedir_is_accessed(e->owner->pagedir, e->spte->vaddr))
			{
				pagedir_set_accessed(e->owner->pagedir, e->spte->vaddr, false);
			}
			else
			{
				//printf("eviction choose end\n");
				return e;
			}
		}
		it = list_next(it);
		if(it == list_end(&frame_table))
		{
			it = list_begin(&frame_table);
		}
	}
	printf("not found!\n");
	lock_release(&frame_lock);
	shutdown_power_off();
	//sys_exit(-1);
	return NULL;
}

struct fte *fte_search(void *vaddr)
{
	//lock_acquire(&frame_lock);
	struct list_elem *e;
	struct fte *found_fte;
	for(e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e))
	{
		found_fte = list_entry(e, struct fte, ft_elem);
		if(found_fte->spte->vaddr == vaddr)
			return found_fte;
	}
	//lock_release(&frame_lock);
	return NULL;
}
