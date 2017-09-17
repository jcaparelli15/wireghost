#include<net/ip.h>
#include<net/tcp.h>
#include<linux/ip.h>
#include<linux/tcp.h>
#include<linux/kernel.h>
#include<linux/module.h>
#include<linux/proc_fs.h>
#include<linux/seq_file.h>
#include<linux/netfilter.h>
#include<linux/etherdevice.h>
#include<linux/netfilter_ipv4.h>
#include "dictionary.h" 

#define procfs_name "wireghost"

// Parameters to be passed to the module
static char * keyToReplace;
static char * replacementForKey;

// Paramter declarations 
module_param(keyToReplace, charp, S_IRUGO | S_IWUSR);
module_param(replacementForKey, charp, S_IRUGO | S_IWUSR);

//Sequence and acknowledgement tables
entry* seqTable[HASH_TABLE_SIZE];
entry* ackTable[HASH_TABLE_SIZE];

static struct nf_hook_ops netfilter_ops_in; //NF_INET_PRE_ROUTING
static struct nf_hook_ops netfilter_ops_out; //NF_INET_POST_ROUTING


//Procfs structures
struct proc_dir_entry *proc_file_entry; 
static const struct file_operations proc_file_fops = {
	.owner = THIS_MODULE,
	.read = seq_read,
};

static inline int ipToInt(int a, int b, int c, int d) {
	/* IP to int is first octet * 256^3 + second octet * 256^2 + third octet * 256 + fourth octet */
	//return (a * 16777216) + (b * 65536) + (c * 256) + d;
	/* Bit shifting is quicker than multiplication
	 * 2^24 = 256^3, 2^16 = 256^2, 256 = 2^8 */
	return (a << 24) + (b << 16) + (c << 8) + d;
}

/* Given payload, replace every occurence of key in payload with replacement, payload modified in place
 * returns the amount of times key was replaced */
int payloadFind(char* payload, const char* key, const char* replacement) {
        char * lastOccurence; //Last occurence of key
        char * nextOccurence; //Next occurence of key
        char * temp; //Temporary array to store the new payload
        int seen; //Amount of times key has been replaced
        temp = kmalloc(1500, GFP_KERNEL);
        seen = 0;
        nextOccurence = strstr(payload, key);
        lastOccurence = (char *)payload;
        while (nextOccurence != NULL) {
                seen++;
                strncat(temp, lastOccurence, nextOccurence-lastOccurence); //Cat to temp from lastOccurence to nextOccurence
                strcat(temp, replacement); //Cat the replacement
                lastOccurence = nextOccurence+strlen(key); //Change lastOccurence to currentOccurence 
                nextOccurence = strstr(nextOccurence+1, key); //nextOccurence to the actual next occurence
        }
        strcat(temp, lastOccurence); //Cat from the last occurence to the end of payload to temp
	strncpy(payload, temp, strlen(temp)); //Assign to payload temp
	payload[strlen(temp)] = '\0'; //Cat the null char
	kfree(temp);
	return seen; //Return replacements 
}

/* Update TCP and IP checksums for a given skb*/
/* THIS DOESNT WORK ON NON LINEAR SKB */
void fixChecksums(struct sk_buff * skb) {
	struct tcphdr *tcph = tcp_hdr(skb);
	struct iphdr *iph = ip_hdr(skb);
	int tcp_len;
	tcp_len = skb->len - 4 * iph->ihl;
	iph->ttl += 1;
	ip_send_check(iph);
	tcph->check = 0;
	tcph->check = tcp_v4_check(tcp_len, iph->saddr, iph->daddr, csum_partial((char *)tcph, tcp_len, 0));
	if (!skb) {
		printk("NETFILTER.C: SKB IS NULL FOR SOME REASON\n");
	}

}

//Modify this function how you please to change what happens to incoming packets
unsigned int in_hook(void *priv, struct sk_buff * skb, const struct nf_hook_state *state) {
	struct sk_buff *nskb;
	/* TCP, IP, and Ethernet headers */
	struct tcphdr *ntcph, *tcph = tcp_hdr(skb);
	struct iphdr *iph = ip_hdr(skb);
	struct ethhdr *nethh, *ethh = eth_hdr(skb);
	/* Structure that will be assigned to the seq and ack tables when needed */
	struct entry t;
	/* char * to end of payload, beginning of payload and an iterator */
	unsigned char *tail, *user_data, *it, *et;
	/* char * to store the payload and the second packet payload */
	char *payload;
	/* Length of original payload, length of tcp header, number of replacements done to payload
	 * the sequence and acknowledgement offsets, multipurpose iterator (for loops, while, etc) */
	int lenOrig, replacements, offset, i;
	/* Source port, destination port and the ip header length */
	__u16 sport, dport, ip_len;
	/* Source IP address and destination IP address */
	__u32 saddr, daddr;
	/* Sequence and acknowledgement numbers */
	uint32_t seq, ack;
	seq = ack = 0;
	payload = kmalloc(1500, GFP_KERNEL);

	/* Ignore the skb if it is empty */
	if (!skb)
		return NF_ACCEPT;

	if (iph->protocol == IPPROTO_ICMP) {
		printk("NETFILTER.C: Ping packet incoming\n");
	}

	/* Convert the source IP address (saddr), source port (sport), 
	   destination IP address (daddr), and destination port (dport)
	   from the network format to the host format */
	saddr = ntohl(iph->saddr);
	daddr = ntohl(iph->daddr);
	sport = ntohs(tcph->source);
	dport = ntohs(tcph->dest);

	/* User_data points to the beginning of the payload in the skb
	   tail points to the end of the payload in skb */
	tail = skb_tail_pointer(skb);
	user_data = (unsigned char *)((unsigned char *)tcph + (tcph->doff * 4));

	/* Filter all IP's except those we are interested in */
	if(saddr == ipToInt(10,0,2,12) || saddr == ipToInt(10,0,2,13)) {
		//skb_unshare(skb, GFP_ATOMIC); //Must unshare the skb to modify it

		/* Loops through the skb payload and stores it in char * payload */
		lenOrig = 0;
		for (it = user_data; it != tail; ++it) {
			char c = *(char *)it;
			payload[lenOrig] = c;
			lenOrig++;
		}
		payload[lenOrig] = '\0';

		/*STORY TIME!!!!
		The following if statement does in fact send packets, hooray!
		The problem is, it sends them to itself and the packets never 
		leave this computer, not hooray! I can get the packets to leave this
		computer by removing the ethernet header though. "But Alberto, if removing
		the ethernet header works, why don't you do it?" Ah my friend, if I do remove it
		then the packets go to the other computers, but because there's no mac address
		A bunch of the packet is cut off because the computer assumes that the beginning
		of the packet is the mac addresses and the protocol, what do I do now?
		I don't know.
		*/

		/* Begin sending of duplicate packet*/
		if (false) { //Change condition to whatever
			//printk("NETFILTER.C: Sending duplicate packet\n");

			/* ALlocate a socket buffer and assign it to be a copy of the
			one passed to us by netfilter */
			nskb = alloc_skb(skb->len, GFP_ATOMIC);
			nskb = pskb_copy(skb, GFP_ATOMIC);

			/*pskb_copy does not give us the ethernet header, so create it */
			nethh = eth_hdr(nskb);

			/*memcpy to the new skb's ethernet header the destination
			address in the original skb, do the same for the source */
			memcpy(nethh->h_dest, ethh->h_dest, ETH_ALEN);
			memcpy(nethh->h_source, ethh->h_source, ETH_ALEN);

			//printk("NETFILTER.C: SOURCE MAC: %x:%x:%x", nethh->h_source[3], nethh->h_source[4], nethh->h_source[5]);
			//printk(" DEST MAC: %x:%x:%x\n", nethh->h_dest[3], nethh->h_dest[4], nethh->h_dest[5]);

			/*Assign the ethernet packets protocol (ETH_P_IP - 0x0800 which is IPv4) */
			nethh->h_proto = ETH_P_IP;

			/* Just to differentiate the duplicates easier, set the first 3 chars to capital H */
			ntcph = tcp_hdr(nskb);
			user_data = (unsigned char *)((unsigned char *)ntcph + (ntcph->doff * 4));
			memcpy(user_data, "HHH", 3);

			//dev_hard_header(nskb, skb->dev, ETH_P_IP, nethh->h_dest, nethh->h_source, ETH_ALEN);

			/* The next four lines accomplish the same thing as
			the dev_hard_header function, neither of which work right */
			et = skb_push(nskb, ETH_HLEN);
			memcpy(et, nethh->h_dest, ETH_ALEN);
			memcpy(et+ETH_ALEN, nethh->h_source, ETH_ALEN);
			nskb->protocol = nethh->h_proto = ntohs(ETH_P_IP);

			i = dev_queue_xmit(nskb);

		} /* Duplicate packet has now been sent*/

		//printk("NETFILTER.C: DATA OF ORIGINAL SKB: %s\n", payload);

		/* If keyToReplace or replacementForKey weren't passed in, don't change the packet
		 * and if we're not changing anything, just accept it anyways */
		if (!keyToReplace || !replacementForKey) {
			return NF_ACCEPT;
		}
		
		/* Change the payload data stored in char * payload */
		replacements = payloadFind(payload, keyToReplace, replacementForKey);

		/* If the acknowledgement table doesn't have an entry for the source IP
		 * create an entry and store it in both the sequence and acknowledgement table 
		 * with a default offset of 0 */
		if (!getVal(seqTable, saddr)) {
			t.ip = saddr;
			t.offset = 0;
			storeVal(ackTable, t);
			storeVal(seqTable, t);
		}
		/* Same as the the above statement except for the destination IP */
		if (!getVal(ackTable, daddr)) {
			t.ip = daddr;
			t.offset = 0;
			storeVal(ackTable, t);
			storeVal(seqTable, t);
		}

		/* Change sequence number from network to host, add offset, back to network */
		seq = ntohl(tcph->seq);
		seq += getVal(seqTable, saddr)->offset;
		tcph->seq = htonl(seq);
		
		/* Change acknowledgement number from network to host, add offset, back to network */
		ack = ntohl(tcph->ack_seq);
		ack += getVal(ackTable, saddr)->offset;
		tcph->ack_seq = htonl(ack);

		/* If the length of the outgoing packet is different from the original payload
		 * the acknowledgement and sequence numbers will be off. This will cause the two 
		 * computers to start arguing amongst each other.  The offset between the correct
		 * acknowledgement and sequence number is the difference between the old payload 
		 * and the new payload + all the past difference.
		 *
		 * So add the new difference to the offset for all future packets */
		if (strlen(payload) != lenOrig) {
			offset = strlen(payload)-lenOrig;
			t.ip = saddr;
			t.offset = getVal(seqTable, saddr)->offset + offset;
			storeVal(seqTable, t);
			t.ip = daddr;
			t.offset = getVal(ackTable, daddr)->offset - offset;
			storeVal(ackTable, t);
		}

		/* Change the size of the skb so that it allows for larger payloads
		   may fail if the changed payload is shorter */
		if(pskb_expand_head(skb, 0, strlen(payload)-skb_tailroom(skb), GFP_ATOMIC)) {
			printk("Sorry, we couldn't expand the skb, you'll just have to accept it\n");
			return NF_ACCEPT;
		}

		/* Refresh header pointers after skb expand */
		tcph = tcp_hdr(skb);
		iph = ip_hdr(skb);

		/* Reserve space in the skb for the data */
		/* THIS BREAKS IF THE PAYLOAD IS NOT ASCII */
		skb_put(skb, strlen(payload)-lenOrig);

		/* memcpy into user_data the modified payload */
		user_data = (unsigned char *)((unsigned char *)tcph + (tcph->doff * 4));
		memcpy(user_data, payload, strlen(payload));

		/* Convert the ip length from the network, change it to the new length, back to network */
		ip_len = ntohs(iph->tot_len);
		ip_len += strlen(payload)-lenOrig;
		iph->tot_len = htons(ip_len);

		/* Update TCP and IP Checksums */
		fixChecksums(skb);

	}
	kfree(payload);
	return NF_ACCEPT;
}
//Modify this function how you please to change what happens to outgoing packets
unsigned int out_hook(void *priv, struct sk_buff * skb, const struct nf_hook_state *state) {
	struct tcphdr * tcph = tcp_hdr(skb);
	struct iphdr * iph = ip_hdr(skb);
	unsigned char *user_data, *it, *tail;
	u32 saddr;

	/* Convert source IP address from network format to host format */
	saddr = ntohl(iph->saddr);

	/* Same as above, user_data points to beginning of payload and tail 
	   points to the end of the payload */
	user_data = (unsigned char *)((unsigned char *)tcph + (tcph->doff * 4));
	tail = skb_tail_pointer(skb);

	/* Filter for only the IP addresses we are interested in */
	if(saddr == ipToInt(10,0,2,12) || saddr == ipToInt(10,0,2,13)) {
		/* Loop through the skb payload and print it */
		//printk("NETFILTER.C: OUTGOING PACKET DATA: ");
		for (it = user_data; it != tail; ++it) {
			char c = *(char *)it;
			if (c == '\0') {
				break;
			}
			//printk("%c", c);
		}
		//printk("\n");

	}
	return NF_ACCEPT; //Let packets go through
}

int init_module() {
	printk("NETFILTER.C: Installing module wireghost\n");
	netfilter_ops_in.hook = in_hook;
	netfilter_ops_in.pf = PF_INET;
	netfilter_ops_in.hooknum = NF_INET_PRE_ROUTING;
	netfilter_ops_in.priority = NF_IP_PRI_FIRST;

	netfilter_ops_out.hook = out_hook;
	netfilter_ops_out.pf = PF_INET;
	netfilter_ops_out.hooknum = NF_INET_POST_ROUTING;
	netfilter_ops_out.priority = NF_IP_PRI_FIRST;

	nf_register_hook(&netfilter_ops_in);
	nf_register_hook(&netfilter_ops_out);

	proc_file_entry = proc_create("wireghost", 0, NULL, &proc_file_fops);
	if (proc_file_entry == NULL) {
		return -ENOMEM;
	}

	return 0;
}

void cleanup_module() {
	nf_unregister_hook(&netfilter_ops_in);
	nf_unregister_hook(&netfilter_ops_out);

	proc_remove(proc_file_entry);
}


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alberto Cabello");
MODULE_DESCRIPTION("A packet modifier that maintains the TCP connection");
