#include "frosted.h"
#include <stdint.h>
#include "uart_dev.h"
#include "uart.h"

#include "libopencm3/cm3/nvic.h"

#ifdef LM3S
#   include "libopencm3/lm3s/usart.h"
#endif
#ifdef STM32F4
#   include "libopencm3/stm32/usart.h"
#   define usart_clear_rx_interrupt(x) do{}while(0)
#   define usart_clear_tx_interrupt(x) do{}while(0)
#endif
#ifdef LPC176X
#   define usart_clear_rx_interrupt(x) do{}while(0)
#   define usart_clear_tx_interrupt(x) do{}while(0)
#endif
struct dev_uart {
    uint32_t base;
    uint32_t irq;
    struct fnode *fno;
    struct cirbuf *inbuf;
    struct cirbuf *outbuf;
    uint8_t *w_start;
    uint8_t *w_end;
    mutex_t *mutex;
    uint16_t pid;
};

#define MAX_UARTS 8

static struct dev_uart DEV_UART[MAX_UARTS];

static struct module mod_devuart = {
};

static int devuart_write(int fd, const void *buf, unsigned int len);

static struct dev_uart *uart_check_fd(int fd)
{
    struct fnode *fno;
    fno = task_filedesc_get(fd);
    
    if (!fno)
        return 0;
    if (fd < 0)
        return 0;

    if (fno->owner != &mod_devuart)
        return 0;
    return fno->priv;
}

void uart_isr(struct dev_uart *uart)
{
    /* TX interrupt */
    if (usart_get_interrupt_source(uart->base, USART_SR_TXE)) {
        /* Are there bytes left to be written? */
        if (cirbuf_bytesinuse(uart->outbuf))
        {
            uint8_t outbyte;
            cirbuf_readbyte(uart->outbuf, &outbyte);
            usart_send(uart->base, (uint16_t)(outbyte));
        } else {
            usart_disable_tx_interrupt(uart->base);
        }
        usart_clear_tx_interrupt(uart->base);
    }

    /* RX interrupt */
    if (usart_get_interrupt_source(uart->base, USART_SR_RXNE)) {
        usart_clear_rx_interrupt(uart->base);
        /* if data available */
        if (usart_is_recv_ready(uart->base))
        {
            char byte = (char)(usart_recv(uart->base) & 0xFF); 
            /* read data into circular buffer */
            cirbuf_writebyte(uart->inbuf, byte);
        }
    }

    /* If a process is attached, resume the process */
    if (uart->pid > 0) 
        task_resume(uart->pid);
}

void uart0_isr(void)
{
    uart_isr(&DEV_UART[0]);
}

void uart1_isr(void)
{
    uart_isr(&DEV_UART[1]);
}

void uart2_isr(void)
{
    uart_isr(&DEV_UART[2]);
}

#ifdef USART0
void usart0_isr(void)
{
    uart_isr(&DEV_UART[0]);
}
#endif

#ifdef USART1
void usart1_isr(void)
{
    uart_isr(&DEV_UART[1]);
}
#endif
#ifdef USART2
void usart2_isr(void)
{
    uart_isr(&DEV_UART[2]);
}
#endif
#ifdef USART3
void usart3_isr(void)
{
    uart_isr(&DEV_UART[3]);
}
#endif

static int devuart_write(int fd, const void *buf, unsigned int len)
{
    int i;
    char *ch = (char *)buf;
    struct dev_uart *uart;

    uart = uart_check_fd(fd);
    if (!uart)
        return -1;
    if (len <= 0)
        return len;
    if (fd < 0)
        return -1;

    if (uart->w_start == NULL) {
        uart->w_start = (uint8_t *)buf;
        uart->w_end = ((uint8_t *)buf) + len;
    } else {
        /* previous transmit not finished, do not update w_start */
    }

    frosted_mutex_lock(uart->mutex);

    /* write to circular output buffer */
    uart->w_start += cirbuf_writebytes(uart->outbuf, uart->w_start, uart->w_end - uart->w_start);

    usart_enable_tx_interrupt(uart->base);

    if (uart->w_start < uart->w_end)
    {
        uart->pid = scheduler_get_cur_pid();
        task_suspend();
        frosted_mutex_unlock(uart->mutex);
        return SYS_CALL_AGAIN;
    }

    frosted_mutex_unlock(uart->mutex);
    uart->w_start = NULL;
    uart->w_end = NULL;
    return len;
}


static int devuart_read(int fd, void *buf, unsigned int len)
{
    int out;
    volatile int len_available;
    char *ptr = (char *)buf;
    struct hal_iodev *dev;
    struct dev_uart *uart;

    if (len <= 0)
        return len;
    if (fd < 0)
        return -1;

    uart = uart_check_fd(fd);
    if (!uart)
        return -1;

    frosted_mutex_lock(uart->mutex);
    usart_disable_rx_interrupt(uart->base);
    len_available =  cirbuf_bytesinuse(uart->inbuf);
    if (len_available <= 0) {
        uart->pid = scheduler_get_cur_pid();
        task_suspend();
        out = SYS_CALL_AGAIN;
        goto again;
    }

    if (len_available < len)
        len = len_available;

    for(out = 0; out < len; out++) {
        /* read data */
        if (cirbuf_readbyte(uart->inbuf, ptr) != 0)
            break;
        ptr++;
    }
    uart->pid = 0;

again:
    usart_enable_rx_interrupt(uart->base);
    frosted_mutex_unlock(uart->mutex);
    return out;
}


static int devuart_poll(int fd, uint16_t events, uint16_t *revents)
{
    int ret = 0;
    struct dev_uart *uart = uart_check_fd(fd);
    if (!uart)
        return -1;
    *revents = 0;
    if (events & POLLOUT) {
        *revents |= POLLOUT;
        ret = 1; /* TODO: implement interrupt for write events */
    }
    if ((events == POLLIN) && usart_is_recv_ready(uart->base)) {
        *revents |= POLLIN;
        ret = 1;
    }
    return ret;
}

static int devuart_open(const char *path, int flags)
{
    struct fnode *f = fno_search(path);
    if (!f)
        return -1;
    return task_filedesc_add(f); 
}


int uart_fno_init(struct fnode *dev, uint32_t n, const struct uart_addr * addr)
{
    struct dev_uart *u = &DEV_UART[n];
    static int num_ttys = 0;

    char name[6] = "ttyS";
    name[4] =  '0' + num_ttys++;

    if (addr->base == 0)
        return -1;

    u->base = addr->base;
    u->irq = addr->irq;

    u->fno = fno_create(&mod_devuart, name, dev);
    u->pid = 0;
    u->mutex = frosted_mutex_init();
    u->inbuf = cirbuf_create(128);
    u->outbuf = cirbuf_create(128);
    u->fno->priv = u;
    usart_enable_rx_interrupt(u->base);
    nvic_enable_irq(u->irq);
    return 0;

}

struct module * devuart_init(struct fnode *dev)
{
    mod_devuart.family = FAMILY_FILE;
    mod_devuart.ops.open = devuart_open;
    mod_devuart.ops.read = devuart_read; 
    mod_devuart.ops.poll = devuart_poll;
    mod_devuart.ops.write = devuart_write;
    
    return &mod_devuart;
}

