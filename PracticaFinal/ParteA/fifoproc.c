#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/vmalloc.h>
#include <asm-generic/uaccess.h>
#include <asm-generic/errno.h>
#include <linux/semaphore.h>
#include <linux/moduleparam.h>
#include "cbuffer.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Milagros del Rocío Peña Quineche");

#define MAX_KBUF		    512
#define MAX_CBUFFER_LEN 	512
#define MAX_NAME_LEN		6

static int fifoX = 2;

module_param(fifoX, int, 0000);
MODULE_PARM_DESC(fifoX, "An integer");

struct list_head my_fifolist;

typedef struct{
	char *name;
	int prod_count;
	int cons_count;
	int nr_prod_waiting;
	int nr_cons_waiting;
	struct semaphore mtx;
	struct semaphore sem_prod;
	struct semaphore sem_cons;
	cbuffer_t *cbuffer;
	struct list_head links;
}fifolist;

struct semaphore sem_fifolist;

static struct proc_dir_entry *proc_fifo;

/* Funciones de inicialización y descarga del módulo */
int init_module(void);
void cleanup_module(void);

static int fifoproc_open(struct inode *inode, struct file *file){
	fifolist *fifo;

	fifo = (fifolist *) PDE_DATA(file->f_inode);
	
	if (file->f_mode & FMODE_READ) {
		if (down_interruptible(&(fifo->mtx))) {
			return -EINTR;
		}
		/* A consumer opens FIFO */
		fifo->cons_count++;
		
		while (fifo->nr_prod_waiting) {
			fifo->nr_prod_waiting--;
			up(&(fifo->sem_prod));
		}
		
		while (fifo->prod_count == 0) {
			fifo->nr_cons_waiting++;
			up(&(fifo->mtx));
			if (down_interruptible(&(fifo->sem_cons))) { 
				down(&(fifo->mtx));
				fifo->nr_cons_waiting--;
				fifo->cons_count--;
				up(&(fifo->mtx));
				return -EINTR;
			}
			if (down_interruptible(&(fifo->mtx))) {
				down(&(fifo->mtx));
				fifo->cons_count--;
				up(&(fifo->mtx));
			}
		}
	} else {
		if (down_interruptible(&(fifo->mtx))) {
			return -EINTR;
		}
		/* A producer opens FIFO */
		fifo->prod_count++;

		while (fifo->nr_cons_waiting) {
			fifo->nr_cons_waiting--;
			up(&(fifo->sem_cons));
		}
		
		while (fifo->cons_count == 0) {
			fifo->nr_prod_waiting++;
			up(&(fifo->mtx));
			if (down_interruptible(&(fifo->sem_prod))) { 
				down(&(fifo->mtx));
				fifo->nr_prod_waiting--;
				fifo->prod_count--;
				up(&(fifo->mtx));
				return -EINTR;
			}
			if (down_interruptible(&(fifo->mtx))) {
				down(&(fifo->mtx));
				fifo->prod_count--;
				up(&(fifo->mtx));
			}
		}
	}
	
	up(&(fifo->mtx));
	
	return 0;
}

/* Se invoca al hacer close() de entrada /proc */
static int fifoproc_release(struct inode *inode, struct file *file){
	fifolist *fifo;

	fifo = (fifolist *) PDE_DATA(file->f_inode);

	if (down_interruptible(&(fifo->mtx))) {
		return -EINTR;
	}
	
	if (file->f_mode & FMODE_READ) {
		fifo->cons_count--;
		

		while (fifo->nr_prod_waiting > 0) {
			up(&(fifo->sem_prod));
			fifo->nr_prod_waiting--;

		}

	} else {
		fifo->prod_count--;

		while (fifo->nr_cons_waiting > 0) {
			up(&(fifo->sem_cons));
			fifo->nr_cons_waiting--;
		}
	}

	if(fifo->cons_count == 0 && fifo->prod_count == 0){
		clear_cbuffer_t(fifo->cbuffer);
	}
	
	up(&(fifo->mtx));
	
	return 0;
}

/* Se invoca al hacer read() de entrada /proc */
static ssize_t fifoproc_read(struct file *filp, char *buf, size_t len, loff_t *off){
	char kbuf[MAX_KBUF];
	fifolist *fifo;

	fifo = (fifolist *) PDE_DATA(filp->f_inode);
	
	if(len > MAX_KBUF || len> MAX_CBUFFER_LEN ) { return -ENOSPC; }
	
	/* Block mutex */
	if(down_interruptible(&(fifo->mtx))) {
		return -EINTR;
	}
		
	/* Block while buffer is empty */
	while (size_cbuffer_t(fifo->cbuffer) < len && fifo->prod_count > 0) {
		/* Increments consumers waiting */
		fifo->nr_cons_waiting++;
		
		up(&(fifo->mtx));
		
		if (down_interruptible(&(fifo->sem_cons))) {
			down(&(fifo->mtx));
			fifo->nr_cons_waiting--;
			up(&(fifo->mtx));
			return -EINTR;
		}
		
		if (down_interruptible(&(fifo->mtx))) {
			return -EINTR;
		}
	}
	
	if (fifo->prod_count == 0 && is_empty_cbuffer_t(fifo->cbuffer)) {
		up(&(fifo->mtx));
		return 0;
	}

	remove_items_cbuffer_t(fifo->cbuffer, kbuf, len);
 
    if(fifo->nr_prod_waiting > 0){
        up(&(fifo->sem_prod));
        fifo->nr_prod_waiting--;
    }

    up(&(fifo->mtx));

    /* Enviamos datos al espacio de ususario */
    if (copy_to_user(buf, kbuf, len))
        return -EINVAL;

    *off+=len;

	return len;
}

/* Se invoca al hacer write() de entrada /proc */
static ssize_t fifoproc_write(struct file *filp, const char *buf, size_t len, loff_t *off){
	char kbuf[MAX_KBUF];
	fifolist *fifo;

	fifo = (fifolist *) PDE_DATA(filp->f_inode);

	if(len> MAX_CBUFFER_LEN || len> MAX_KBUF) { 
		printk(KERN_INFO "Fifoproc: not enough space!\n");
		return -ENOSPC; 
	}
	
	/* Transfer data from user to kernel space */
    if (copy_from_user(kbuf, buf, len ))
        return -EFAULT;

    kbuf[len] = '\0';

	/* Block mutex */
	if (down_interruptible(&(fifo->mtx))) { return -EINTR; }
	
	/* Producer is blocked if there isn't space */
	while (nr_gaps_cbuffer_t(fifo->cbuffer) < len && fifo->cons_count > 0) {
		/* Increase producers waiting */
		fifo->nr_prod_waiting++;
		
		/* Releases the mutex before blocking it */
		up(&(fifo->mtx));
		
		/* Blocking queue */
		if (down_interruptible(&(fifo->sem_prod))) {
			down(&(fifo->mtx));
			fifo->nr_prod_waiting--;
			up(&(fifo->mtx));
			return -EINTR;
		}
		
		/* Adquires the mutex before entering the critical section */
		if (down_interruptible(&(fifo->mtx))) {
			return -EINTR;
		}
	}
	
	if (fifo->cons_count == 0) {
		up(&(fifo->mtx));
		return -EPIPE;
	}

	/* Insert data received in cbuffer */
	insert_items_cbuffer_t(fifo->cbuffer, kbuf, len);
	
	/* Wake up to potential consumers */
    if(fifo->nr_cons_waiting > 0){
        up(&(fifo->sem_cons));
        fifo->nr_cons_waiting--;
    }
	
	/* Unlock mutex */
	up(&(fifo->mtx));

	*off+=len;
	
	return len;
}


static const struct file_operations proc_entry_fops = {
	.open = fifoproc_open,
	.release = fifoproc_release,
    .read = fifoproc_read,
    .write = fifoproc_write,    
};

int add_fifoproc(void){
	int i;
	fifolist *node;
	char *aux;

	if(fifoX < 2 || fifoX > 10){
		return -EINVAL;
	}

	for(i = 0; i < fifoX; i++){
		node = (fifolist *)vmalloc(sizeof(fifolist));
		node->name = (char *)vmalloc(MAX_NAME_LEN * sizeof(char));
		
		aux = "";
		
		sprintf(aux, "fifo%d", i); 
		strcpy(node->name, aux);
		node->cons_count = 0;
		node->prod_count = 0;
		node->nr_cons_waiting = 0;
		node->nr_prod_waiting = 0;
		sema_init(&(node->mtx), 1);
		sema_init(&(node->sem_prod), 0);
		sema_init(&(node->sem_cons), 0);
		node->cbuffer = create_cbuffer_t(MAX_CBUFFER_LEN);

		if(down_interruptible(&sem_fifolist)){
			printk(KERN_INFO "Fifoproc: Couldn't create /proc entry");
			return -ENOMEM;
		}

		list_add_tail(&node->links, &my_fifolist);

		up(&sem_fifolist);

		proc_fifo = proc_create_data(aux, 0666, NULL, &proc_entry_fops, (void *) node);
		if(proc_fifo == NULL){
        	printk(KERN_INFO "Error al crear /proc/fifo%d.\n", i);
        	return -EFAULT;
        }
	}
	return 0;
}

int remove_fifoprocs(void){
	struct list_head *node = NULL;
	struct list_head *aux = NULL;
	fifolist *fifo;

	if(down_interruptible(&sem_fifolist)){
		return -ENOMEM;
	}

	list_for_each_safe(node, aux, &my_fifolist){
		fifo = list_entry(node, fifolist, links);
		destroy_cbuffer_t(fifo->cbuffer);
		list_del(node);
		remove_proc_entry(fifo->name, NULL);

		vfree(fifo->name);
		vfree(fifo);
	}

	up(&sem_fifolist);

	return 0;
}


int init_fifoproc_module(void) {

	sema_init(&sem_fifolist, 1);
	INIT_LIST_HEAD(&my_fifolist);
	add_fifoproc();

	printk(KERN_INFO "Fifoproc: Module loaded.\n");
	
	return 0;
}

void exit_fifoproc_module(void) {
	remove_fifoprocs();

	printk(KERN_INFO "Fifoproc: Module unloaded.\n");
}


module_init( init_fifoproc_module );
module_exit( exit_fifoproc_module );
