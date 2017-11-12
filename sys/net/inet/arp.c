#include <net/ether/ether.h>
#include <net/inet/arp.h>
#include <net/inet/util.h>
#include <net/inet/params.h>
#include <net/inet/protohdr.h>
#include <net/util.h>
#include <kern/lock.h>
#include <kern/kernlib.h>
#include <kern/pktbuf.h>
#include <kern/netdev.h>
#include <kern/task.h>
#include <kern/timer.h>

struct pending_frame {
  struct list_head link;
  struct pktbuf *frm;
  u16 proto;
  struct netdev *dev;
};

struct arpentry {
  struct etheraddr macaddr;
  in_addr_t ipaddr;
  u16 timeout;
#define ARPTBL_PERMANENT 0xffff //timeoutをこの値にした時はタイムアウトしない
  struct list_head pending; //アドレス解決待ちのフレーム
};

struct arpentry arptable[MAX_ARPTABLE];
static int next_register = 0; //次の登録位置

enum arpresult {
  RESULT_FOUND      = 0,
  RESULT_NOT_FOUND  = 1,
  RESULT_ADD_LIST    = 2,
};

static mutex arptbl_mtx;

static void arp_10sec(void *);

NET_INIT void arp_init() {
  mutex_init(&arptbl_mtx);

  for(int i=0;i<MAX_ARPTABLE;i++)
    list_init(&arptable[i].pending);

  defer_exec(arp_10sec, NULL, 0, 10*SEC);
}

static struct pending_frame *pending_frame_new(struct pktbuf *frm, u16 proto, struct netdev *dev) {
  struct pending_frame *p = malloc(sizeof(struct pending_frame));
  p->frm = frm;
  p->proto = proto;
  p->dev = dev;
  return p;
}

static void pending_frame_free(struct pending_frame *p) {
  pktbuf_free(p->frm);
  free(p);
}

static void pending_remove_all(struct list_head *pending) {
  struct list_head *p, *tmp;
  list_foreach_safe(p, tmp, pending) {
    struct pending_frame *frm = list_entry(p, struct pending_frame, link);
    list_remove(p);
    pending_frame_free(frm);
  }
}

static int arp_resolve(in_addr_t ipaddr, struct etheraddr *macaddr, struct pktbuf *frm, u16 proto, struct netdev *dev) {
  mutex_lock(&arptbl_mtx);

  for(int i=0; i<MAX_ARPTABLE; i++){
    if(arptable[i].ipaddr == ipaddr &&  arptable[i].timeout>0){
      int result;
      if(list_is_empty(&arptable[i].pending)){
        //保留無し->アドレス解決できてる
        *macaddr = arptable[i].macaddr;
        result = RESULT_FOUND;
      }else{
        //保留となっているフレームが他にもある
        //受付順に送信するため、末尾に挿入
        list_pushback(&pending_frame_new(frm, proto, dev)->link, &arptable[i].pending);
        result = RESULT_ADD_LIST;
      }
      mutex_unlock(&arptbl_mtx);
      return result;
    }
  }

  //登録なしなので、IPアドレスだけ登録。保留リストに入れる
  if(!list_is_empty(&arptable[next_register].pending)){
    pending_remove_all(&arptable[next_register].pending);
  }
  list_pushfront(&pending_frame_new(frm, proto, dev)->link, &arptable[next_register].pending);
  arptable[next_register].timeout = ARBTBL_TIMEOUT_CLC;
  arptable[next_register].ipaddr = ipaddr;
  next_register = (next_register+1) % MAX_ARPTABLE;

  mutex_unlock(&arptbl_mtx);
  return RESULT_NOT_FOUND;
}

void register_arptable(in_addr_t ipaddr, struct etheraddr macaddr, int is_permanent){
  mutex_lock(&arptbl_mtx);

  //IPアドレスだけ登録されている（アドレス解決待ち）エントリを探す
  for(int i=0; i<MAX_ARPTABLE; i++) {
    if(arptable[i].ipaddr == ipaddr && arptable[i].timeout>0){
      arptable[i].timeout = is_permanent ? ARPTBL_PERMANENT : ARBTBL_TIMEOUT_CLC; //延長
      if(!list_is_empty(&arptable[i].pending)){
        arptable[i].macaddr = macaddr;
        struct list_head *p;
        list_foreach(p, &arptable[i].pending) {
          struct pending_frame *pending = list_entry(p, struct pending_frame, link);
          ether_tx(pending->frm, macaddr, pending->proto, pending->dev);
        }
        pending_remove_all(&arptable[i].pending);
      }
      mutex_unlock(&arptbl_mtx);
      return;
    }
  }
  if(!list_is_empty(&arptable[next_register].pending))
    pending_remove_all(&arptable[next_register].pending);
  arptable[next_register].timeout = is_permanent?ARPTBL_PERMANENT:ARBTBL_TIMEOUT_CLC;
  arptable[next_register].ipaddr = ipaddr;
  arptable[next_register].macaddr = macaddr;
  next_register = (next_register+1) % MAX_ARPTABLE;
  mutex_unlock(&arptbl_mtx);
  return;
}

void arp_rx(struct pktbuf *frm){
  struct ether_arp *earp = (struct ether_arp *)frm->head;
  if(pktbuf_get_size(frm) < sizeof(struct ether_arp) ||
    ntoh16(earp->arp_hrd) != ARPHRD_ETHER ||
    ntoh16(earp->arp_pro) != ETHERTYPE_IP ||
    earp->arp_hln != ETHER_ADDR_LEN || earp->arp_pln != 4 ||
    (ntoh16(earp->arp_op) != ARPOP_REQUEST && ntoh16(earp->arp_op) !=ARPOP_REPLY) ){
    pktbuf_free(frm);
    return;
  }

  switch(ntoh16(earp->arp_op)){
  case ARPOP_REQUEST:
  {
    struct netdev *dev = NULL;
    struct list_head *p;
    list_foreach(p, &ifaddr_tbl[PF_INET]) {
      struct ifaddr_in *inaddr = 
         list_entry(p, struct ifaddr_in, family_link);
      if(inaddr->addr == earp->arp_tpa) {
        dev = inaddr->dev;
        break;
      }
    }
    if(dev == NULL) {
      pktbuf_free(frm);
      break;
    }

    register_arptable(earp->arp_spa, earp->arp_sha, 0);

    struct etheraddr destether = earp->arp_sha;
    earp->arp_tha = earp->arp_sha;
    earp->arp_tpa = earp->arp_spa;
    earp->arp_sha = *(struct etheraddr *)(netdev_find_addr(dev, PF_LINK)->addr);
    earp->arp_spa = ((struct ifaddr_in *)netdev_find_addr(dev, PF_INET))->addr;
    earp->arp_op = hton16(ARPOP_REPLY);
    ether_tx(frm, destether, ETHERTYPE_ARP, dev);
    break;
  }
  case ARPOP_REPLY:
    register_arptable(earp->arp_spa, earp->arp_sha, 0);
    break;
  }
  return;
}

static void send_arprequest(in_addr_t dstaddr, struct netdev *dev){
  struct pktbuf *req =
    pktbuf_alloc(sizeof(struct ether_hdr) + sizeof(struct ether_arp));

  pktbuf_reserve_headroom(req, sizeof(struct ether_hdr) + sizeof(struct ether_arp));
  struct ether_arp *earp =
   (struct ether_arp *) pktbuf_add_header(req, sizeof(struct ether_arp));

  earp->arp_hrd = hton16(ARPHRD_ETHER);
  earp->arp_pro = hton16(ETHERTYPE_IP);
  earp->arp_hln = ETHER_ADDR_LEN;
  earp->arp_pln = 4;
  earp->arp_op = hton16(ARPOP_REQUEST);
  earp->arp_sha = *(struct etheraddr *)netdev_find_addr(dev, PF_LINK)->addr;
  earp->arp_spa = ((struct ifaddr_in *)netdev_find_addr(dev, PF_INET))->addr;
  memset(&earp->arp_tha, 0x00, ETHER_ADDR_LEN);
  earp->arp_tpa = dstaddr;

  ether_tx(req, ETHER_ADDR_BROADCAST, ETHERTYPE_ARP, dev);
}

static void arp_10sec(void *arg UNUSED) {
  mutex_lock(&arptbl_mtx);
  for(int i=0; i<MAX_ARPTABLE; i++) {
    if(arptable[i].timeout > 0 && 
       arptable[i].timeout != ARPTBL_PERMANENT) {
      arptable[i].timeout--;
    }
    if(arptable[i].timeout == 0) {
      if(!list_is_empty(&arptable[i].pending)) {
        struct list_head *p, *tmp;
        list_foreach_safe(p, tmp, &arptable[i].pending) {
          struct pktbuf *pkt = list_entry(p, struct pktbuf, link);
          pktbuf_free(pkt);
          list_remove(p);
        }
      }
    } else {
      if(!list_is_empty(&arptable[i].pending)) {
        // TODO リスト先頭要素の取り出し関数:
        send_arprequest(arptable[i].ipaddr, list_entry(arptable[i].pending.next, struct pending_frame, link)->dev);
      }
    }
  }
  mutex_unlock(&arptbl_mtx);
  defer_exec(arp_10sec, NULL, 0, 10*SEC);
}

void arp_tx(struct pktbuf *pkt, in_addr_t dstaddr, u16 proto, struct netdev *dev){
  struct etheraddr dest_ether;

  switch(arp_resolve(dstaddr, &dest_ether, pkt, proto, dev)){
  case RESULT_FOUND:
    ether_tx(pkt, dest_ether, proto, dev);
    break;
  case RESULT_NOT_FOUND:
    send_arprequest(dstaddr, dev);
    break;
  case RESULT_ADD_LIST:
    break;
  }
}

