#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/slab.h> // for qset, quantum
#include <linux/cdev.h> // for cdev
#include <linux/uaccess.h> // for copy_to_user

#include "scull.h"
#define DEBUG 1
MODULE_LICENSE("Dual BSD/GPL");

int myint=3;
MODULE_PARM_DESC(myint, "An integer");
module_param(myint, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

int scull_major =   SCULL_MAJOR;
int scull_minor =   0;
int scull_nr_devs = SCULL_NR_DEVS;  /* number of bare scull devices */
int scull_quantum = SCULL_QUANTUM;
int scull_qset =    SCULL_QSET;

struct scull_dev *scull_devices;    /* allocated in scull_init_module */

int scull_release(struct inode * inode, struct file *filp) {
    return 0;
}
int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *next, *dptr;
    int qset = dev->qset;   /* "dev" is not-null */
    int i;

    for (dptr = dev->pqset; dptr; dptr = next) { /* all the list items */
        if (dptr->data) {
            for (i = 0; i < qset; i++)
                kfree(dptr->data[i]);
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);
    }
    dev->size = 0;
    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    dev->pqset = NULL;
    return 0;
}

int scull_open(struct inode  * inode, struct file * filp) {
    struct scull_dev * dev;
    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev;
    
    if (( filp->f_flags & O_ACCMODE ) == O_WRONLY) {
        scull_trim(dev);
    }
    return 0;
}

/*
 * Follow the list
 */
struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->pqset;

        /* Allocate first qset explicitly if need be */
    if (! qs) {
        qs = dev->pqset = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
        if (qs == NULL)
            return NULL;  /* Never mind */
        memset(qs, 0, sizeof(struct scull_qset));
    }

    /* Then follow the list */
    while (n--) {
        if (!qs->next) {
            qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if (qs->next == NULL)
                return NULL;  /* Never mind */
            memset(qs->next, 0, sizeof(struct scull_qset));
        }
        qs = qs->next;
        continue;
    }
    return qs;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM; /* value used in "goto out" statements */

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;

    /* find listitem, qset index and offset in the quantum */
    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;
    s_pos = rest / quantum; q_pos = rest % quantum;

    /* follow the list up to the right position */
    dptr = scull_follow(dev, item);
    if (dptr == NULL)
        goto out;
    if (!dptr->data) {
        dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
        if (!dptr->data)
            goto out;
        memset(dptr->data, 0, qset * sizeof(char *));
    }
    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto out;
    }
    /* write only up to the end of this quantum */
    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_from_user(dptr->data[s_pos]+q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

        /* update the size */
    if (dev->size < *f_pos)
        dev->size = *f_pos;

  out:
    up(&dev->sem);
    return retval;
}

ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;    /* the first listitem */
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset; /* how many bytes in the listitem */
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    if (*f_pos >= dev->size)
        goto out;
    if (*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    /* find listitem, qset index, and offset in the quantum */
    // item = points to nth linked list node. 
    // rest =  points to byte offset within the node data size. 
    // node data size is essentially itemsize.

    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;

    // s_pos: array index.

    s_pos = rest / quantum; 

    // q_pos: index within quantum.

    q_pos = rest % quantum;

    /* follow the list up to the right position (defined elsewhere) */
    dptr = scull_follow(dev, item);

    if (dptr == NULL || !dptr->data || ! dptr->data[s_pos])
        goto out; /* don't fill holes */

    /* read only up to the end of this quantum */

    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

  out:
    up(&dev->sem);
    return retval;
}

struct file_operations scull_fops = {
    .owner =    THIS_MODULE,
    //.llseek =   scull_llseek,
    .read =     scull_read,
    //.write =    scull_write,
    //.unlocked_ioctl = scull_ioctl,
    .open =     scull_open,
    .release =  scull_release,
};

/*
 * Set up the char_dev structure for this device.
 */
static void scull_setup_cdev(struct scull_dev *dev, int index)
{
    printk(KERN_INFO "scull_setup_cdev: entered...");
    int err, devno = MKDEV(scull_major, scull_minor + index);

    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &scull_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    /* Fail gracefully if need be */
    if (err)
        printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

static int scull_init(void) {
    printk(KERN_ALERT "scull, world. Build No. 1\n");
    printk(KERN_INFO "myint is an integer: %d\n", myint);

    int result, i;
    dev_t dev = 0;

    if (scull_major) {
            dev = MKDEV(scull_major, scull_minor);
            result = register_chrdev_region(dev, scull_nr_devs, "scull");
    } else {
            scull_major = MAJOR(dev);
    }

    result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs,  "scull");

    if (result < 0) {
        printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
        return result;
    } else {
        printk(KERN_INFO "acquired major number: %d\n", result);
    }

     /*
     * allocate the devices -- we can't have them static, as the number
     * can be specified at load time
     */

    scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
    if (!scull_devices) {
        result = -ENOMEM;
        goto fail;  /* Make this more graceful */
    }
    memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

    /* Initialize each device. */

    for (i = 0; i < scull_nr_devs; i++) {
        scull_devices[i].quantum = scull_quantum;
        scull_devices[i].qset = scull_qset;
    /* causes build error, comment out for now !!!!GGGG */
        //init_MUTEX(&scull_devices[i].sem);
        init_rwsem(&scull_devices[i].sem);
        scull_setup_cdev(&scull_devices[i], i);
    }

    /* At this point call the init function for any friend device */

    dev = MKDEV(scull_major, scull_minor + scull_nr_devs);
//  dev += scull_p_init(dev);
//  dev += scull_access_init(dev);

fail:
    //scull_exit();
    return 0;
}

static void scull_exit(void) {
    dev_t devno = MKDEV(scull_major, scull_minor);
    unregister_chrdev_region(devno, scull_nr_devs);
    printk(KERN_ALERT "Goodbye, cruel world.\n");
    return;
}

module_init(scull_init);
module_exit(scull_exit);


