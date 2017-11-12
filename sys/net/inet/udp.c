#include <net/inet/udp.h>
#include <net/inet/protohdr.h>
#include <net/inet/ip.h>
#include <net/inet/util.h>
#include <net/inet/params.h>
#include <net/socket.h>
#include <net/util.h>
#include <kern/kernlib.h>
#include <kern/lock.h>
#include <kern/task.h>


#define NEED_PORT_ALLOC 0

struct udpcb {
  struct list_head link;
  struct queue_head recv_queue;
  int recv_waiting;
  struct sockaddr_in addr;
  struct sockaddr_in partner_addr;
};


static void *udp_sock_init();
static int udp_sock_bind(void *pcb, const struct sockaddr *addr);
static int udp_sock_close(void *pcb);
static int udp_sock_connect(void *pcb, const struct sockaddr *addr);
static int udp_sock_sendto(void *pcb, const u8 *msg, size_t len, int flags, struct sockaddr *dest_addr);
static void udp_analyze(struct pktbuf *pkt, struct sockaddr_in *addr);
static int udp_sock_recvfrom(void *pcb, u8 *buf, size_t len, int flags, struct sockaddr *from_addr);
static int udp_sock_send(void *pcb, const u8 *msg, size_t len, int flags);
static int udp_sock_recv(void *pcb, u8 *buf, size_t len, int flags);
static int udp_sock_listen(void *pcb, int backlog);
static int udp_sock_accept(void *pcb, struct sockaddr *client_addr);

static const struct socket_ops udp_sock_ops = {
  .init = udp_sock_init,
  .bind = udp_sock_bind,
  .close = udp_sock_close,
  .connect = udp_sock_connect,
  .listen = udp_sock_listen,
  .accept = udp_sock_accept,
  .sendto = udp_sock_sendto,
  .recvfrom = udp_sock_recvfrom,
  .send = udp_sock_send,
  .recv = udp_sock_recv,
};

static struct list_head udpcb_list;

static mutex udp_mtx;
static mutex udp_recv_mtx;

#define UDPCB(s) (((struct udpcb *)(s))->pcb)

NET_INIT void udp_init() {
  list_init(&udpcb_list);

  mutex_init(&udp_mtx);
  mutex_init(&udp_recv_mtx);

  socket_add_ops(PF_INET, SOCK_DGRAM, &udp_sock_ops);
}

static int is_used_port(in_port_t port) {
  struct list_head *p;
  list_foreach(p, &udpcb_list) {
    struct udpcb *cb = list_entry(p, struct udpcb, link);
    if(cb->addr.port == port)
      return 1;
  }

  return 0;
}

static in_port_t get_unused_port() {
	//already locked.
	for(in_port_t p=49152; p<65535; p++)
		if(!is_used_port(p))
			return p;

	return 0;
}

static u16 udp_checksum(struct ip_hdr *iphdr, struct udp_hdr *uhdr) {
  struct udp_pseudo_hdr pseudo;
  pseudo.up_src = iphdr->ip_src;
  pseudo.up_dst = iphdr->ip_dst;
  pseudo.up_type = 17;
  pseudo.up_void = 0;
  pseudo.up_len = uhdr->uh_ulen; //UDPヘッダ+UDPペイロードの長さ

  return checksum2((u16*)(&pseudo), (u16*)uhdr, sizeof(struct udp_pseudo_hdr), ntoh16(uhdr->uh_ulen));
}

static void set_udpheader(struct udp_hdr *uhdr, u16 seglen, in_port_t sport, struct sockaddr_in *dest_addr){
  uhdr->uh_sport = hton16(sport);
  uhdr->uh_dport = hton16(dest_addr->port);
  uhdr->uh_ulen = hton16(seglen);
  uhdr->sum = 0;

  struct ip_hdr iphdr_tmp;
  iphdr_tmp.ip_src = IPADDR;
  iphdr_tmp.ip_dst = dest_addr->addr;

  uhdr->sum = udp_checksum(&iphdr_tmp, uhdr);

  return;
}

void udp_rx(struct pktbuf *pkt, struct ip_hdr *iphdr){
  struct udp_hdr *uhdr = (struct udp_hdr *)pkt->head;

  if(pktbuf_get_size(pkt) < sizeof(struct udp_hdr) ||
    pktbuf_get_size(pkt) != ntoh16(uhdr->uh_ulen)){
    goto exit;
  }

  if(uhdr->sum != 0 && udp_checksum(iphdr, uhdr) != 0)
    goto exit;

  if(uhdr->uh_dport == 0)
    goto exit;

  mutex_lock(&udp_mtx);

  struct udpcb *cb = NULL;
  struct list_head *p;
  list_foreach(p, &udpcb_list) {
    struct udpcb *b = list_entry(p, struct udpcb, link);
    if(b->addr.port == ntoh16(uhdr->uh_dport)) {
      cb = b;
      break;
    }
  }
  if(cb == NULL)
    goto exit;

  mutex_lock(&udp_recv_mtx);
  if(queue_is_full(&cb->recv_queue)) {
    pktbuf_free(list_entry(queue_dequeue(&cb->recv_queue), struct pktbuf, link));
  }
  pktbuf_add_header(pkt, ip_header_len(iphdr)); //IPヘッダも含める
  queue_enqueue(&pkt->link, &cb->recv_queue);

  if(cb->recv_waiting) task_wakeup(socket);
  mutex_unlock(&udp_recv_mtx);
  mutex_unlock(&udp_mtx);
  return;

exit:
  mutex_unlock(&udp_mtx);
  pktbuf_free(pkt);
  return;
}


static void *udp_sock_init() {
  struct udpcb *cb = malloc(sizeof(struct udpcb));
  queue_init(&cb->recv_queue, UDP_RECVQUEUE_LEN);
  cb->recv_waiting = 0;
  bzero(&cb->addr, sizeof(struct sockaddr_in));
  bzero(&cb->partner_addr, sizeof(struct sockaddr_in));
  list_pushback(&cb->link, &udpcb_list);
  return cb;
}

static int udp_sock_bind(void *pcb, const struct sockaddr *addr) {
  struct udpcb *cb = (struct udpcb *)pcb;
  if(addr->family != PF_INET)
    return -1;

  struct sockaddr_in *inaddr = (struct sockaddr_in *)addr;
  if(inaddr->port == NEED_PORT_ALLOC)
    inaddr->port = get_unused_port();
  else if(is_used_port(inaddr->port))
    return -1;

  memcpy(&cb->addr, inaddr, sizeof(struct sockaddr_in));
  return 0;
}

static int udp_sock_close(void *pcb) {
  struct udpcb *cb = (struct udpcb *)pcb;
  mutex_lock(&udp_recv_mtx);
  while(!queue_is_empty(&cb->recv_queue))
    pktbuf_free(list_entry(queue_dequeue(&cb->recv_queue), struct pktbuf, link));
  list_remove(&cb->link);
  mutex_unlock(&udp_recv_mtx);
  return 0;
}

static int udp_sock_connect(void *pcb, const struct sockaddr *addr) {
  struct udpcb *cb = (struct udpcb *)pcb;
  if(addr->family != PF_INET)
    return -1;

  struct sockaddr_in *inaddr = (struct sockaddr_in *)addr;
  if(inaddr->port == NEED_PORT_ALLOC);
    return -1;

  memcpy(&cb->partner_addr, inaddr, sizeof(struct sockaddr_in));
  return 0;
}

static int udp_sock_sendto(void *pcb, const u8 *msg, size_t len, int flags UNUSED, struct sockaddr *dest_addr){
  struct udpcb *cb = (struct udpcb *)pcb;

  if(dest_addr->family != PF_INET)
    return -1;
  if(0xffff-sizeof(struct udp_hdr) < len)
    return -1;

  struct pktbuf *udpseg = pktbuf_alloc(sizeof(struct udp_hdr) + len);
  pktbuf_copyin(udpseg, msg, len, sizeof(struct udp_hdr));

  if(cb->addr.port == NEED_PORT_ALLOC)
    cb->addr.port = get_unused_port();

  set_udpheader((struct udp_hdr *)udpseg->head, pktbuf_get_size(udpseg), cb->addr.port, (struct sockaddr_in *)dest_addr);

  ip_tx(udpseg, ((struct sockaddr_in *)dest_addr)->addr, IPTYPE_UDP);

  return len;
}

static void udp_analyze(struct pktbuf *pkt, struct sockaddr_in *addr) {
  struct ip_hdr *iphdr=(struct ip_hdr *)pkt->head;
  struct udp_hdr *udphdr = (struct udp_hdr *)(((u8 *)iphdr)+ip_header_len(iphdr));
  if(addr != NULL) {
    addr->addr = iphdr->ip_src;
    addr->port = ntoh16(udphdr->uh_sport);
  }
  return;
}

static int udp_sock_recvfrom(void *pcb, u8 *buf, size_t len, int flags UNUSED, struct sockaddr *from_addr) {
  struct udpcb *cb = (struct udpcb *)pcb;
  mutex_lock(&udp_recv_mtx);
  cb->recv_waiting = 1;
  while(1) {
    if(queue_is_empty(&cb->recv_queue)) {
      mutex_unlock(&udp_recv_mtx);
      task_sleep();
    } else {
      struct pktbuf *pkt = list_entry(queue_dequeue(&cb->recv_queue), struct pktbuf, link);
      udp_analyze(pkt, (struct sockaddr_in *)from_addr); //FIXME: check size of from_addr.
      pktbuf_remove_header(pkt, sizeof(struct udp_hdr));
      size_t copied = MIN(len, pktbuf_get_size(pkt));
      memcpy(buf, pkt->head, copied);
      pktbuf_free(pkt);
      cb->recv_waiting = 0;
      mutex_unlock(&udp_recv_mtx);
      return copied;
    }

    mutex_lock(&udp_recv_mtx);
  }
}

static int udp_sock_send(void *pcb, const u8 *msg, size_t len, int flags) {
  struct udpcb *cb = (struct udpcb *)pcb;
  return udp_sock_sendto(pcb, msg, len, flags, (struct sockaddr *)(&cb->partner_addr));
}

static int udp_sock_recv(void *pcb, u8 *buf, size_t len, int flags) {
  return udp_sock_recvfrom(pcb, buf, len, flags, NULL);
}

static int udp_sock_listen(void *pcb UNUSED, int backlog UNUSED) {
  return -1;
}

static int udp_sock_accept(void *pcb UNUSED, struct sockaddr *client_addr UNUSED) {
  return -1;
}
