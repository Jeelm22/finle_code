#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/module.h>
#include "ioctl_commands.h"

#define DEVICE_NAME "dm510_dev"
#define BUFFER_SIZE 1024
#define MINOR_START 0
#define DEVICE_COUNT 2
#define DM510_IOC_MAGIC 'k'
#define DM510_IOCRESET _IO(DM510_IOC_MAGIC, 0)
#define DM510_IOCSQUANTUM _IOW(DM510_IOC_MAGIC, 1, int)

static int dm510_major = 0;
module_param(dm510_major, int, S_IRUGO);

struct buffer {
    char *data;
    int size;
    int head, tail;
    struct semaphore sem;
    wait_queue_head_t read_queue, write_queue;
};

static struct buffer shared_buffer; 

struct dm510_device {
    struct cdev cdev;
    struct buffer *shared_buffer;
    int nreaders, nwriters;
    int max_processes; // New field to limit the number of processes
};

static struct dm510_device device[DEVICE_COUNT];

static int dm510_open(struct inode *inode, struct file *filp) {
    struct dm510_device *dev =  container_of(inode->i_cdev, struct dm510_device, cdev);
    filp->private_data = dev;

    if (down_interruptible(&shared_buffer.sem))
        return -ERESTARTSYS;
    switch (filp->f_flags & O_ACCMODE) {
	    // Will be denind writing acces, becues device is busy
        case O_WRONLY:
            if (dev->nwriters) {
                up(&shared_buffer.sem);
                return -EBUSY;
            }
            dev->nwriters++;
            break;
        case O_RDONLY:
		// Will be dening access, becues there are to many readers
            if (dev->nreaders >= dev->max_processes) {
                up(&shared_buffer.sem);
                return -EMFILE;
            }
            dev->nreaders++;
            break;
        case O_RDWR:
		// Will be denine read/write access becuse device is busy
            if (dev->nwriters || (dev->nreaders > 0 && dev->nreaders >= dev->max_processes)) {
                up(&shared_buffer.sem);
                return (dev->nwriters) ? -EBUSY : -EMFILE;
            }
            // This needs to check max_processes for readers as well
	    // Will be denine access becues there are too many readers
            dev->nwriters++;
            dev->nreaders++;
            break;
    }
    up(&shared_buffer.sem);
    return 0;
}


static int dm510_release(struct inode *inode, struct file *filp) {
    struct dm510_device *dev = filp->private_data;


    down(&shared_buffer.sem);
    if ((filp->f_flags & O_ACCMODE) == O_WRONLY || (filp->f_flags & O_ACCMODE) == O_RDWR) {
        dev->nwriters--;
    }
    if ((filp->f_flags & O_ACCMODE) == O_RDONLY || (filp->f_flags & O_ACCMODE) == O_RDWR) {
        dev->nreaders--;
    }
    up(&shared_buffer.sem);
    return 0;
}


ssize_t dm510_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    struct dm510_device *dev = filp->private_data;
    struct buffer *shared_buf = dev->shared_buffer;

    if (down_interruptible(&shared_buf->sem))
        return -ERESTARTSYS;

    while (shared_buf->head == shared_buf->tail) { // Buffer is empty
        up(&shared_buf->sem); // Release the semaphore to allow writers to proceed
        if (filp->f_flags & O_NONBLOCK) {
            return -EAGAIN; // If non-blocking mode, return immediately
        }
        if (wait_event_interruptible(shared_buf->read_queue, shared_buf->head != shared_buf->tail))
            return -ERESTARTSYS; // Wait for data to be written
        if (down_interruptible(&shared_buf->sem))
            return -ERESTARTSYS;
    }

    // Calculate the amount of readable data
    size_t available = (shared_buf->tail > shared_buf->head) ? (shared_buf->tail - shared_buf->head) : (shared_buf->size - shared_buf->head + shared_buf->tail);
    count = min(count, available);

    size_t first_part_size = min(count, (size_t)(shared_buf->size - shared_buf->head)); // Cast to size_t

    if (copy_to_user(buf, shared_buf->data + shared_buf->head, first_part_size)) {
        up(&shared_buf->sem);
        return -EFAULT;
    }

    // If data wraps around to the beginning of the buffer
    if (count > first_part_size) {
        if (copy_to_user(buf + first_part_size, shared_buf->data, count - first_part_size)) {
            up(&shared_buf->sem);
            return -EFAULT;
        }
    }

    // Update head pointer with wrap around
    shared_buf->head = (shared_buf->head + count) % shared_buf->size;

    wake_up_interruptible(&shared_buf->write_queue); // Wake up waiting writers if space has been freed up

    up(&shared_buf->sem);
    return count;
}

ssize_t dm510_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
    struct dm510_device *dev = filp->private_data;
    struct buffer *shared_buf = dev->shared_buffer;

    if (down_interruptible(&shared_buf->sem))
        return -ERESTARTSYS;

    size_t space_available = (shared_buf->head > shared_buf->tail) ? 
                             (shared_buf->head - shared_buf->tail - 1) : 
                             (shared_buf->size - shared_buf->tail + shared_buf->head - 1);

    while (space_available == 0) { // Changed to while for reevaluation after wake up
        if (filp->f_flags & O_NONBLOCK) {
            up(&shared_buf->sem); // Release the semaphore before returning
            return -EAGAIN; // Non-blocking operation should return immediately
        }
        // For blocking I/O, wait until there is space in the buffer
        up(&shared_buf->sem); // Release the semaphore before going to sleep

        if (wait_event_interruptible(shared_buf->write_queue,
            (shared_buf->head > shared_buf->tail) ? 
            (shared_buf->head - shared_buf->tail - 1) : 
            (shared_buf->size - shared_buf->tail + shared_buf->head - 1) > 0)) {
            // If the wait is interrupted by a signal, return -ERESTARTSYS
            return -ERESTARTSYS;
        }

        if (down_interruptible(&shared_buf->sem))
            return -ERESTARTSYS;
        
        // Recalculate space_available after waking up
        space_available = (shared_buf->head > shared_buf->tail) ? 
                          (shared_buf->head - shared_buf->tail - 1) : 
                          (shared_buf->size - shared_buf->tail + shared_buf->head - 1);
    }

    // Limit write size to available space in the buffer to prevent overwrite
    count = min(count, space_available);

    size_t first_part_size = min(count, (size_t)(shared_buf->size - shared_buf->tail)); // Cast to size_t
    if (copy_from_user(shared_buf->data + shared_buf->tail, buf, first_part_size)) {
        up(&shared_buf->sem);
        return -EFAULT;
    }

    // If data wraps around to the beginning of the buffer
    if (count > first_part_size) {
        if (copy_from_user(shared_buf->data, buf + first_part_size, count - first_part_size)) {
            up(&shared_buf->sem);
            return -EFAULT;
        }
    }

    // Update tail pointer with wrap around
    shared_buf->tail = (shared_buf->tail + count) % shared_buf->size;

    // Wake up readers waiting for data
    wake_up_interruptible(&shared_buf->read_queue);

    up(&shared_buf->sem);
    return count;
}


long dm510_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct dm510_device *dev = filp->private_data;
    int new_size, retval = 0;
    switch (cmd) {
        case GET_BUFFER_SIZE:
            if (copy_to_user((int __user *)arg, &shared_buffer.size, sizeof(shared_buffer.size)))
                retval = -EFAULT;
            break;
	case SET_BUFFER_SIZE:
	    if (copy_from_user(&new_size, (int __user *)arg, sizeof(new_size))) {
        	retval = -EFAULT;
	    } else if (new_size < 5) { // Ensure minimum buffer size of 5 bytes
        	retval = -EINVAL; // Invalid buffer size
	    } else {
        	char *new_buffer = kzalloc(new_size * sizeof(char), GFP_KERNEL);
        	if (!new_buffer) {
	            retval = -ENOMEM; // Out of memory
	        } else {
	            down(&shared_buffer.sem); // Ensure exclusive access to the buffer
	            //  printk(KERN_INFO "DM510: Current buffer size is %d bytes, new size is %d bytes.\n", shared_buffer.size, new_size);
            	    kfree(shared_buffer.data); // Free old buffer
            	    shared_buffer.data = new_buffer; // Assign new buffer
            	    shared_buffer.size = new_size; // Update buffer size
            	    shared_buffer.head = 0; // Reset pointers
            	    shared_buffer.tail = 0;
            	    up(&shared_buffer.sem); // Release the semaphore
            	    retval = 0; // Indicate success
        	}
    	    }
   	    break;

       case GET_MAX_NR_PROCESSES:
           if (copy_to_user((int __user *)arg, &dev->max_processes, sizeof(dev->max_processes))) {
               retval = -EFAULT;
	  } 
          break;

        case SET_MAX_NR_PROCESSES:
            // Updating max_processes from the value provided by user space
            if (copy_from_user(&dev->max_processes, (int __user *)arg, sizeof(dev->max_processes))) {
                retval = -EFAULT;
            }
            break;

        case GET_BUFFER_FREE_SPACE: { 
            int free_space;
            down(&shared_buffer.sem); // Ensure exclusive access
            if (shared_buffer.tail >= shared_buffer.head) {
                free_space = shared_buffer.size - (shared_buffer.tail - shared_buffer.head) - 1;
            } else {
                free_space = (shared_buffer.head - shared_buffer.tail) - 1;
            }
            up(&shared_buffer.sem);
            if (copy_to_user((int __user *)arg, &free_space, sizeof(free_space))) {
                retval = -EFAULT;
            }
            break;
	}

        case GET_BUFFER_USED_SPACE: {
            int used_space;
            down(&shared_buffer.sem); // Ensure exclusive access
            if (shared_buffer.tail >= shared_buffer.head) {
                used_space = shared_buffer.tail - shared_buffer.head;
            } else {
                used_space = shared_buffer.size - (shared_buffer.head - shared_buffer.tail);
            }
            up(&shared_buffer.sem);
            if (copy_to_user((int __user *)arg, &used_space, sizeof(used_space))) {
                retval = -EFAULT;
            }
	    break;
	}
                default:
                    retval = -ENOTTY;
	   }
    	   return retval;
}

static struct file_operations dm510_fops = {
    .owner = THIS_MODULE,
    .open = dm510_open,
    .release = dm510_release,
    .read = dm510_read,
    .write = dm510_write,
    .unlocked_ioctl = dm510_ioctl,
};

static void dm510_setup_cdev(struct dm510_device *dev, int index) {
    int err;
    dev_t devno = MKDEV(dm510_major, MINOR_START + index);

    // Set up the char device
    cdev_init(&dev->cdev, &dm510_fops);
    dev->cdev.owner = THIS_MODULE;

    // Add the char device
    err = cdev_add(&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_NOTICE "Error %d adding DM510 device", err);
        return;
    }

    // Point the device's shared_buffer pointer to the actual shared_buffer
    dev->shared_buffer = &shared_buffer;
}

static void buffer_init(struct dm510_device *dev) {
    // Initialize device-specific fields
    sema_init(&shared_buffer.sem, 1);
    dev->nreaders = 0;
    dev->nwriters = 0;
    dev->max_processes = 1;
    // Point to the shared buffer
    dev->shared_buffer = &shared_buffer;
}

static int __init dm510_init(void) {
    int result, i;
    dev_t dev = 0;
    //Initialize shared_buffer
    shared_buffer.data = kzalloc(BUFFER_SIZE * sizeof(char), GFP_KERNEL);
    if (!shared_buffer.data) {
        // Handle memory allocation error
        printk(KERN_WARNING "DM510: Unable to allocate shared buffer\n");
        return -ENOMEM;
    }
    shared_buffer.size = BUFFER_SIZE;
    sema_init(&shared_buffer.sem, 1);
    init_waitqueue_head(&shared_buffer.read_queue);
    init_waitqueue_head(&shared_buffer.write_queue);
    shared_buffer.head = 0;
    shared_buffer.tail = 0;

    if (dm510_major) {
        dev = MKDEV(dm510_major, MINOR_START);
        result = register_chrdev_region(dev, DEVICE_COUNT, DEVICE_NAME);
    } else {
        result = alloc_chrdev_region(&dev, MINOR_START, DEVICE_COUNT, DEVICE_NAME);
        dm510_major = MAJOR(dev);
    }
    if (result < 0) {
        printk(KERN_WARNING "DM510: can't get major %d\n", dm510_major);
        return result;
    }

    for (i = 0; i < DEVICE_COUNT; ++i) {
        buffer_init(&device[i]);
        dm510_setup_cdev(&device[i], i);
    }
    return 0;
}

static void __exit dm510_cleanup(void) {
    int i;
    for (i = 0; i < DEVICE_COUNT; ++i) {
        cdev_del(&device[i].cdev);
        
    }
    unregister_chrdev_region(MKDEV(dm510_major, MINOR_START), DEVICE_COUNT);
    kfree(shared_buffer.data);
}

module_init(dm510_init);
module_exit(dm510_cleanup);

MODULE_AUTHOR("Your Name");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DM510 Assignment Device Driver");
