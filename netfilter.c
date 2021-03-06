#include<net/ip.h>
#include<net/tcp.h>
#include<linux/ip.h>
#include<linux/tcp.h>
#include<linux/kernel.h>
#include<linux/module.h>
#include<linux/sysctl.h>
#include<linux/netfilter.h>
#include<linux/etherdevice.h>
#include<linux/netfilter_ipv4.h>
#include "dictionary.h" 

#define MAX_KEY_LENGTH 400
#define MAX_REPLACEMENT_LENGTH 400
#define MAX_INJECTION_STRING_LENGTH 1500
#define MAX_INJECTION_STACK_SIZE 100
#define MAX_MANGLE_SIZE 10
#define BITECOIN 17

//Sequence and acknowledgement tables
entry* seqTable[HASH_TABLE_SIZE];
entry* ackTable[HASH_TABLE_SIZE];


//Injection queue
int injectionStackSize = 0;
char *injectionStack[MAX_INJECTION_STACK_SIZE] = {NULL};

//Mangle keys array
int mangleSize = 0;
char *mangleKeys[MAX_MANGLE_SIZE] = {NULL};
char *mangleValues[MAX_MANGLE_SIZE] = {NULL};

static struct nf_hook_ops netfilter_ops_in; //NF_INET_PRE_ROUTING
static struct nf_hook_ops netfilter_ops_out; //NF_INET_POST_ROUTING


int wireghost_nl_go(char *command) {
	char *delim = strchr(command+2, ':');
	if (command[0] == 'i') {
		//Safeguard overflowing the array
		if (injectionStackSize > MAX_INJECTION_STACK_SIZE - 1) {
			return -1;
		}
		else {
			//Add to injection queue the thing to inject which starts at the third character to the end
			injectionStack[injectionStackSize] = kmalloc(1000, GFP_KERNEL);
			strcpy(injectionStack[injectionStackSize], command+2);
			//Ask how they want this, does every injected packet send new line?
			strcat(injectionStack[injectionStackSize], "\n");
			injectionStackSize++;
			return 1;
		}
	}
	if (command[0] == 'm') {
		//Safeguard overflowing the array
		if (mangleSize > MAX_MANGLE_SIZE - 1) {
			return -2;
		}
		else {
			//Add to the mangle "dictionary"
			mangleKeys[mangleSize] = kmalloc(1000, GFP_KERNEL);
			strncpy(mangleKeys[mangleSize], command+2, delim-(command+2));
			mangleKeys[mangleSize][delim-(command+2)] = '\0';
			mangleValues[mangleSize] = kmalloc(1000, GFP_KERNEL);
			strcpy(mangleValues[mangleSize], delim+1);
			mangleSize++;
			return 2;
		}
	}
	if (command[0] == 'c') {
		if (command[2] == 'i') { //Clear injection stack
			while (injectionStackSize) {
				kfree(injectionStack[injectionStackSize-1]);
				injectionStack[injectionStackSize-1] = NULL;
				injectionStackSize--;
			}
			return 3;
		}
		if (command[2] == 'm') { //Clear mangle dictionary
			while(mangleSize) {
				kfree(mangleKeys[mangleSize-1]);
				mangleKeys[mangleSize-1] = NULL;
				kfree(mangleValues[mangleSize-1]);
				mangleValues[mangleSize-1] = NULL;
				mangleSize--;
			}
			return 4;
		}
	}

	return 1;
}

//Begin netlink socket functions and structures
struct sock *nl_sk = NULL;
void wireghost_nl_recv_msg(struct sk_buff *skb) {
	struct nlmsghdr *nlh;	
	int pid;
	struct sk_buff *skb_out;
	int msg_size;
	char msg[500];
	int res;
	int parseResult = 0;

	
	nlh = (struct nlmsghdr *)skb->data;
//	printk("NETFILTER.C: Netlink message received payload: %s\n", (char *)NLMSG_DATA(nlh));
	//Parse userspace input
	parseResult = wireghost_nl_go(NLMSG_DATA(nlh));
	if (parseResult < 0) {
		if (parseResult == -1) {
			strcpy(msg, "Injection queue overflowed, nothing was added so this will never be injected, please wait for the queue to empty\n");
		}
		if (parseResult == -2) {
			strcpy(msg, "No more space in the mangling dictionary, update will not take place\n");
		}
	}
	else {
		if (parseResult == 1) {
			strcpy(msg, "Injection Successful\n");
		}
		if (parseResult == 2) {
			strcpy(msg, "Mangle trigger added\n");
		}
		if (parseResult == 3) {
			strcpy(msg, "Injection stack cleared\n");
		}
		if (parseResult == 4) {
			strcpy(msg, "Mangling dictionary cleared\n");
		}
	}
	msg_size = strlen(msg);
	//Send userspace confirmation
	pid = nlh->nlmsg_pid;

	skb_out = nlmsg_new(msg_size, 0);
	if (!skb_out) {
		printk("Failed to allocate new SKB\n");
		return;
	}
	nlh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
	NETLINK_CB(skb_out).dst_group = 0;
	strcpy(NLMSG_DATA(nlh), msg);
	res = nlmsg_unicast(nl_sk, skb_out, pid);

	if (res < 0) {
		printk("NETFILTER.C: Error sending back the netlink socket to userspace\n");
	}

}
		
		



//Support function, converts IP addresses to intgers
static inline int ipToInt(int a, int b, int c, int d) {
	/* IP to int is first octet * 256^3 + second octet * 256^2 + third octet * 256 + fourth octet */
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

int injectNewPacket(struct sk_buff *orig, unsigned char *sourceKey, unsigned char *destKey, 
		    const struct nf_hook_state *state, char *message, int lenOrig) {
	struct entry t;
	struct sk_buff *skb;
	struct tcphdr *ntcph;
	struct iphdr *iph;
	unsigned char *user_data;
	uint32_t seq, ack;
	int offset;
	__u16 ip_len;


	/* Allocate a socket buffer and assign it to be a copy of the
	one passed to us by netfilter */
	skb = skb_copy(orig, GFP_ATOMIC);

	/*Create pointer to tcp header*/
	ntcph = tcp_hdr(skb);

	//Copying the packet resets the ttl and fixChecksum() adds 1
	//to it causing a ttl of 65
	ip_hdr(skb)->ttl -= 1;

	seq = ntohl(ntcph->seq);
	seq += getVal(seqTable, sourceKey)->offset;
	ntcph->seq = htonl(seq);

	ack = ntohl(ntcph->ack_seq);
	ack += getVal(ackTable, sourceKey)->offset;
	ntcph->ack_seq = htonl(ack);

	/* Just to differentiate the duplicates easier, change packet to message
	* again, may fail if payload is shorter*/
	if(pskb_expand_head(skb, 0, strlen(message)-skb_tailroom(skb), GFP_ATOMIC)) {
		return -ENOMEM;
	}
	
	//Refresh tcp header after expand
	ntcph = tcp_hdr(skb);
	iph = ip_hdr(skb);

	/* Reserve space in the skb for the data */
	/* THIS BREAKS IF THE PAYLOAD IS NOT ASCII */
	skb_put(skb, strlen(message)-lenOrig);

	/* memcpy into user_data the modified payload */
	user_data = (unsigned char *)((unsigned char *)ntcph + (ntcph->doff * 4));
	memcpy(user_data, message, strlen(message));

	/* Convert the ip length from the network, change it to the new length, back to network */
	ip_len = ntohs(iph->tot_len);
	ip_len += strlen(message)-lenOrig;
	iph->tot_len = htons(ip_len);

	ntcph = tcp_hdr(skb);
	user_data = (unsigned char *)((unsigned char *)ntcph + (ntcph->doff * 4));
	memcpy(user_data, message, strlen(message));

	offset = skb_tail_pointer(skb)-user_data;
	t.ip = sourceKey;
	t.offset = getVal(seqTable, sourceKey)->offset + offset;
	storeVal(seqTable, t);
	t.ip = destKey;
	t.offset = getVal(ackTable, destKey)->offset - offset;
	storeVal(ackTable, t);

	fixChecksums(skb);

	return state->okfn(dev_net(orig->dev), NULL, skb);
	//return nf_hook(NFPROTO_IPV4, NF_INET_LOCAL_OUT, dev_net(orig->dev), NULL, skb, NULL, skb_dst(skb)->dev, dst_output);
}
	
//Modify this function how you please to change what happens to incoming packets
unsigned int in_hook(void *priv, struct sk_buff * skb, const struct nf_hook_state *state) {
	/* TCP, IP, and Ethernet headers */
	struct tcphdr *tcph = tcp_hdr(skb);
	struct iphdr *iph = ip_hdr(skb);
	/* Structure that will be assigned to the seq and ack tables when needed */
	struct entry t;
	/* char * to end of payload, beginning of payload and an iterator */
	unsigned char *tail, *user_data, *it, *sourceKey, *destKey;
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
	sourceKey = kmalloc(25, GFP_KERNEL);
	destKey = kmalloc(25, GFP_KERNEL);

	/* Ignore the skb if it is empty */
	if (!skb)
		return NF_ACCEPT;
	

	/* Convert the source IP address (saddr), source port (sport), 
	   destination IP address (daddr), and destination port (dport)
	   from the network format to the host format */
	saddr = ntohl(iph->saddr);
	daddr = ntohl(iph->daddr);
	sport = ntohs(tcph->source);
	dport = ntohs(tcph->dest);
	
	sprintf(sourceKey, "%u:%u", saddr, sport);
	sprintf(destKey, "%u:%u", daddr, dport);


	/* User_data points to the beginning of the payload in the skb
	   tail points to the end of the payload in skb */
	tail = skb_tail_pointer(skb);
	user_data = (unsigned char *)((unsigned char *)tcph + (tcph->doff * 4));

	/* Filter all IP's except those we are interested in */
	if(saddr == ipToInt(10,0,2,12) || saddr == ipToInt(10,0,2,13)) {
		/* Loops through the skb payload and stores it in char * payload */
		lenOrig = 0;
		for (it = user_data; it != tail; ++it) {
			char c = *(char *)it;
			payload[lenOrig] = c;
			lenOrig++;
		}
		payload[lenOrig] = '\0';

		/* If keyToReplace or replacementForKey weren't passed in, don't change the packet
		 * and if we're not changing anything, just accept it anyways */
//		if (!strlen(keyToReplace) || !strlen(replacementForKey)) {
//			return NF_ACCEPT;
//		}

		/* Check if payload matches any mangle key */
		for (i = 0; i < mangleSize; i++) {
			printk("NETFILTER.C: Checkinf if match with %s\n", mangleKeys[i]);
			if (strstr(payload, mangleKeys[i]) != NULL) {
				printk("NETFILTER.C: Mangling the packet with the match %s\n", mangleKeys[i]);
				replacements = payloadFind(payload, mangleKeys[i], mangleValues[i]);
			}
		}

		/* Change the payload data stored in char * payload */
//		replacements = payloadFind(payload, keyToReplace, replacementForKey);

		/* If the acknowledgement table doesn't have an entry for the source IP
		 * create an entry and store it in both the sequence and acknowledgement table 
		 * with a default offset of 0. */
		if (!getVal(seqTable, sourceKey)) {
			t.ip = sourceKey;
			t.offset = 0;
			storeVal(ackTable, t);
			storeVal(seqTable, t);
		}
		/* Same as the the above statement except for the destination IP */
		if (!getVal(ackTable, destKey)) {
			t.ip = destKey;
			t.offset = 0;
			storeVal(ackTable, t);
			storeVal(seqTable, t);
		}

		while (injectionStackSize) { //Change condition to whatever
			printk("NETFILTER.C: Inject a packet\n");
			injectNewPacket(skb, sourceKey, destKey, state, injectionStack[injectionStackSize-1], lenOrig);
			kfree(injectionStack[injectionStackSize-1]);
			injectionStack[injectionStackSize-1] = NULL;
			injectionStackSize--;
		}

		/* Change sequence number from network to host, add offset, back to network */
		seq = ntohl(tcph->seq);
		seq += getVal(seqTable, sourceKey)->offset;
		tcph->seq = htonl(seq);
		
		/* Change acknowledgement number from network to host, add offset, back to network */
		ack = ntohl(tcph->ack_seq);
		ack += getVal(ackTable, sourceKey)->offset;
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
			t.ip = sourceKey;
			t.offset = getVal(seqTable, sourceKey)->offset + offset;
			storeVal(seqTable, t);
			t.ip = destKey;
			t.offset = getVal(ackTable, destKey)->offset - offset;
			storeVal(ackTable, t);
		}

		/* In order for future connections to be able to be setup
		 * clear the table entry (set it to 0) and future connections
		 * will work */
		if (tcph->fin) {
			t.ip = sourceKey;
			t.offset = 0;
			storeVal(ackTable, t);
			storeVal(seqTable, t);
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
	u32 saddr, daddr;
	int lenOrig = 0;
	char * payload = kmalloc(1500, GFP_KERNEL);;

	/* Convert source IP address from network format to host format */
	saddr = ntohl(iph->saddr);
	daddr = ntohl(iph->daddr);

	/* Same as above, user_data points to beginning of payload and tail 
	   points to the end of the payload */
	user_data = (unsigned char *)((unsigned char *)tcph + (tcph->doff * 4));
	tail = skb_tail_pointer(skb);

	/* Filter for only the IP addresses we are interested in */
	if(saddr == ipToInt(10,0,2,12) || saddr == ipToInt(10,0,2,13)) {
		/* Loop through the skb payload and print it */
		lenOrig = 0;
		for (it = user_data; it != tail; ++it) {
			char c = *(char *)it;
			payload[lenOrig] = c;
			lenOrig++;
		}
		payload[lenOrig] = '\0';
		printk("NETFILTER.C: OUTGOING PACKET DATA: %s MARK: %d\n", payload, skb->mark);

	}
	return NF_ACCEPT; //Let packets go through
}

int init_module() {
	struct netlink_kernel_cfg cfg = {
		.input = wireghost_nl_recv_msg,
	};
	printk("NETFILTER.C: Installing module wireghost\n");
	netfilter_ops_in.hook = in_hook;
	netfilter_ops_in.pf = PF_INET;
	netfilter_ops_in.hooknum = NF_INET_PRE_ROUTING;
	netfilter_ops_in.priority = NF_IP_PRI_LAST;

	netfilter_ops_out.hook = out_hook;
	netfilter_ops_out.pf = PF_INET;
	netfilter_ops_out.hooknum = NF_INET_POST_ROUTING;
	netfilter_ops_out.priority = NF_IP_PRI_FIRST;

	nf_register_hook(&netfilter_ops_in);
//	Uncomment this to regist the post routing hook
//	nf_register_hook(&netfilter_ops_out);
	
	//Start socket
	nl_sk = netlink_kernel_create(&init_net, BITECOIN, &cfg);
	if (!nl_sk) {
		printk("Error creating socket, no wireghost for you\n");
		return -10;
	}
	//Socket started

	return 0;
}

void cleanup_module() {
	nf_unregister_hook(&netfilter_ops_in);
//	Uncomment this if you have the post routing hook
//	nf_unregister_hook(&netfilter_ops_out);

	//Release socket
	netlink_kernel_release(nl_sk);

}


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alberto Cabello");
MODULE_DESCRIPTION("A packet modifier that maintains the TCP connection");
