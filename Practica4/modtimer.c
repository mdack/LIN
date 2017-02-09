#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm-generic/uaccess.h>
#include <linux/ftrace.h>
#include <linux/spinlock_types.h>
#include <linux/semaphore.h>
#include <linux/timer.h>
#include <linux/random.h>
#include <linux/workqueue.h>
#include <asm/atomic.h>
#include <linux/smp.h>
#include <linux/list.h>
#include "cbuffer.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Milagros del Rocío Peña Quineche y Miriam López Sierra");

#define MAX_KBUF 100
#define MAX_CBUFFER_LEN 10


struct proc_dir_entry *proc_entry_mc;
struct proc_dir_entry *proc_entry_mt;

int timer_period_ms = HZ; //indica milisegundos con los que se activará temporizador
int max_random = 250;
int emergency_threshold = 80; //Porcentaje de ocupacion que provoca la activacion de la tarea de vaciado del buffer

cbuffer_t* cbuffer;

/* Timer */
struct timer_list my_timer;
struct work_struct my_work;

int flagFin;

int cons_count=0;
/* Workqueue descriptor */
static struct workqueue_struct *my_wq;  
static struct list_head my_list;
int nr_list_waiting = 0;
unsigned long flags;

/* Nodos de la lista */
typedef struct {
	unsigned int data;
	struct list_head links;
}list_item_t;


DEFINE_SPINLOCK(sp);   // Para el buffer circular
DEFINE_SEMAPHORE(sem1);
struct semaphore sem2;   // Para el acceso al buffer circular

///////////////////////////////////////////////////////////////// LISTA ////////////////////////////////////////////////////////////////////////////////////
void clear_list(struct list_head *list){
  list_item_t *aux, *elem = NULL;

  down(&sem1);
  list_for_each_entry_safe(elem, aux, list, links){
    list_del(&(elem->links));
    vfree(elem);
  }
  up(&sem1);

}

/////////////////////////////////////////////////////////////////WORKQUEUE////////////////////////////////////////////////////////////////////////////////////

static void my_wq_function( struct work_struct *work )
{
    int porcentaje = (MAX_CBUFFER_LEN * emergency_threshold)/ 100;

    unsigned char nAleatorio[MAX_CBUFFER_LEN];
    int i, j=0, tam;
    list_item_t  *num = NULL;

    spin_lock_irqsave(&sp, flags);

    tam = size_cbuffer_t(cbuffer);

    for(i = 0 ; i < tam; i++){
        nAleatorio[i] = remove_cbuffer_t(cbuffer);
    }

    spin_unlock_irqrestore(&sp, flags);

    while( j< tam){
        num=(list_item_t *)vmalloc(sizeof(list_item_t));
        num->data = nAleatorio[j];

        if(down_interruptible(&sem1)){ return; }
        list_add_tail(&num->links, &my_list);
        up(&sem1);
        j++;
    }

    printk(KERN_INFO "%d elementos guardados en la lista\n\n", porcentaje);    
    if(down_interruptible(&sem1)){ return ; }

    if(nr_list_waiting > 0){
        up(&sem2);
        nr_list_waiting--;
    }

    up(&sem1);
    flagFin = 1;
}

///////////////////////////////////////////////////////////////// TIMER ////////////////////////////////////////////////////////////////////////////////////

/* Función que se ejecutará cuando temporizador expire */
static void fire_timer(unsigned long data){ 
    unsigned int ocupado;
    unsigned int aleatorio = get_random_int() % max_random;
    int tam;
    int act_cpu;

    ocupado = (emergency_threshold * MAX_CBUFFER_LEN) / 100;

    if(is_full_cbuffer_t (cbuffer) == 0){
        // Insertamos número en el buffer y comprobamos su tamaño

        spin_lock_irqsave(&sp, flags);

        insert_cbuffer_t (cbuffer, aleatorio);
        tam = size_cbuffer_t(cbuffer);

        spin_unlock_irqrestore(&sp, flags);

        printk(KERN_INFO "modtimer: Generated %d\n", aleatorio);


        // Si se ha alcanzado el tamaño límite lanzamos trabajo diferido
        if(tam >= ocupado && flagFin){
            act_cpu = smp_processor_id();

            // lanzamos trabajo diferido en la otra cpu
            schedule_work_on(act_cpu == 1 ? 0 : 1,&my_work);
            flagFin = 0;
        }
    }else{
        printk(KERN_INFO " LLENO\n");
    }

    mod_timer(&my_timer, jiffies + timer_period_ms);
  
}
///////////////////////////////////////////////////////////////// MODTIMER ////////////////////////////////////////////////////////////////////////////////////

static ssize_t modtimer_read(struct file *file, char *buf, size_t len, loff_t *off){
    list_item_t *lista = NULL;
    list_item_t *aux=NULL;
    char kbuf[MAX_KBUF];
    char *dest=kbuf;

    if(down_interruptible(&sem1)) { return -EINTR; }

    while(list_empty(&my_list)) {
        nr_list_waiting++;
        up(&sem1);

        if (down_interruptible(&sem2)){ 
            if(down_interruptible(&sem1)) return -EINTR;
            nr_list_waiting--;
            up(&sem1);
            return -EINTR;
        }
    
    }
    
    if(down_interruptible(&sem1)){ return -EINTR; }

      
    list_for_each_entry_safe(lista, aux, &my_list, links){   
        dest += sprintf(dest, "%d\n", lista->data);
        list_del(&lista->links);
        vfree(lista);   
    }
    dest += sprintf(dest, "\n");
    up(&sem1);

    if (copy_to_user(buf, kbuf, dest-kbuf))
        return -EINVAL;

    
    *off+=len;

    return (dest-kbuf);
}

static int modtimer_open(struct inode *inode, struct file *file){

    printk(KERN_INFO "hola\n");
    cbuffer =create_cbuffer_t(MAX_CBUFFER_LEN);
    init_timer(&my_timer);
    
    /* Initialize field */
    my_timer.data=0;
    my_timer.function=fire_timer;
    my_timer.expires=jiffies + timer_period_ms; 
   
    add_timer(&my_timer);

    /* Initialize work structure (with function) */
    INIT_WORK(&my_work, my_wq_function );
    INIT_LIST_HEAD(&my_list);
    sema_init(&sem2, 0);
    nr_list_waiting=1;

      /* Create a private workqueue named 'my_queue' */
    my_wq = create_workqueue("my_queue");      

    if (!my_wq){
        printk(KERN_INFO "Error al crear workqueue.\n");
        return -ENOMEM;
    }
    flagFin = 1;
    try_module_get(THIS_MODULE);

    return 0;
}

static int modtimer_release(struct inode *inode, struct file *file){
/* Wait until completion of the timer function (if it's currently running) and delete timer */
    del_timer_sync(&my_timer);
       /* Wait until all jobs scheduled so far have finished */
    flush_workqueue( my_wq );
      
      /* Destroy workqueue resources */
    destroy_workqueue( my_wq );
    destroy_cbuffer_t(cbuffer);
    flush_scheduled_work();
    clear_list(&my_list);

    module_put(THIS_MODULE);


    return 0;
}
//////////////////////////////////////////////// MODCONFIG /////////////////////////////////////////////////////////////////////

static ssize_t modconfig_read(struct file *file, char *buf, size_t len, loff_t *off){
    char kbuf[MAX_KBUF];
    char* dest = kbuf; //Puntero para recorrer el buffer

    

    if ((*off) > 0)
        return 0;
    
    dest += sprintf(dest, "timer_period_ms = %d\nemergency_threshold = %d\nmax_random = %d\n", timer_period_ms, emergency_threshold, max_random);

    /* Enviamos datos al espacio de ususario */
    if (copy_to_user(buf, kbuf,  dest-kbuf))
        return -EINVAL;

    (*off)+=len;  /* Update the file pointer */
    return (dest-kbuf);
}

static ssize_t modconfig_write(struct file *file, const char *buf, size_t len, loff_t *off){
    int num;
    char command[25];
    char kbuf[MAX_KBUF];

    if ((*off) > 0)
        return 0;

    // leemos la cadena de caracteres del espacio de usuario
    if (copy_from_user( &kbuf[0], buf, len ))
        return -EFAULT;

    kbuf[len] = '\0';

    sscanf(&kbuf[0],"%s %d", command, &num);

    if(strcmp(command, "timer_period_ms") == 0) {
        timer_period_ms = num;
    }else if(strcmp(command, "emergency_threshold") == 0) {
         if(num > 100){ 
            printk(KERN_INFO "El argumento debe ser menor que 100\n\n");
            return -EINVAL;
        }
        emergency_threshold = num;
    }else if(strcmp(command, "max_random") == 0) {
        if(num > 255){ 
            printk(KERN_INFO " EL argumento debe ser menor de 256\n\n");
            return -EINVAL;
        }
        max_random = num;
    }else {
        return -EINVAL;
    }

    *off+=len;

    return len;
}


static const struct file_operations proc_entry_fops_modconfig = {
    .read = modconfig_read,
    .write = modconfig_write,
};

static const struct file_operations proc_entry_fops_modtimer = {
    .read = modtimer_read,
    .open = modtimer_open,
    .release = modtimer_release,
};

int init_modtimer_module( void )
{

    proc_entry_mc = proc_create("modconfig",0666, NULL, &proc_entry_fops_modconfig);
	 
    if(proc_entry_mc == NULL){
        printk(KERN_INFO " Error al cargar modconfig.\n");
        return -EFAULT;

    }else{
        printk(KERN_INFO "Cargado modconfig.\n");
        proc_entry_mt = proc_create("modtimer",0666, NULL, &proc_entry_fops_modtimer);
        
        if(proc_entry_mt == NULL) { 
            printk(KERN_INFO " Error al cargar modtimer.\n");
            return -EFAULT;
        }
        printk(KERN_INFO "Modulos cargados\n");
     
    }
    
    return 0;
    

}


void cleanup_modtimer_module( void ){

printk(KERN_INFO"Modulos descargados\n");

  remove_proc_entry("modtimer", NULL);
  remove_proc_entry("modconfig", NULL);
}

module_init( init_modtimer_module );
module_exit( cleanup_modtimer_module );