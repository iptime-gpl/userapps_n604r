/* FTP extension for IP connection tracking. */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/ip.h>
#include <linux/ctype.h>
#include <net/checksum.h>
#include <net/tcp.h>

#if defined(CONFIG_RTL8186_TR) || defined(CONFIG_RTL8196B_TR) || defined(CONFIG_RTL8196B_GW)
#include <linux/proc_fs.h>
#endif
//#ifdef CONFIG_RTL865X_AC
#if defined(CONFIG_RTL865X_AC) || defined(CONFIG_RTL865X_KLD) || defined(CONFIG_RTL8196B_KLD)
#include <linux/proc_fs.h>
#endif

#include <linux/netfilter_ipv4/lockhelp.h>
#include <linux/netfilter_ipv4/ip_conntrack_helper.h>
#include <linux/netfilter_ipv4/ip_conntrack_ftp.h>
#ifdef CONFIG_RTL_KERNEL_MIPS16_NETFILTER
#include <asm/mips16_lib.h>
#endif

#undef printk

#ifdef CONFIG_IP_NF_ALGCTRL
int ip_ct_alg_ftp = 1;
#endif

DECLARE_LOCK(ip_ftp_lock);
struct module *ip_conntrack_ftp = THIS_MODULE;

#define MAX_PORTS 8
static int ports[MAX_PORTS];
static int ports_c = 0;
#ifdef MODULE_PARM
MODULE_PARM(ports, "1-" __MODULE_STRING(MAX_PORTS) "i");
#endif

static int loose = 0;
MODULE_PARM(loose, "i");

#if 0
#define DEBUGP printk
#else
#define DEBUGP(format, args...)
#endif

#if defined(CONFIG_RTL8186_TR) || defined(CONFIG_RTL8196B_TR) || defined(CONFIG_RTL8196B_GW)
#define FTP_CONNTRACK_NAT_UPDATE
#endif
//#ifdef CONFIG_RTL865X_AC
#if defined(CONFIG_RTL865X_AC) || defined(CONFIG_RTL865X_KLD) || defined(CONFIG_RTL8196B_KLD)
#define FTP_CONNTRACK_NAT_UPDATE
#endif

#ifdef FTP_CONNTRACK_NAT_UPDATE
struct proc_dir_entry *resb=NULL;
#endif //FTP_CONNTRACK_NAT_UPDATE

static int try_rfc959(const char *, size_t, u_int32_t [], char);
static int try_eprt(const char *, size_t, u_int32_t [], char);
static int try_epsv_response(const char *, size_t, u_int32_t [], char);

static struct ftp_search {
	enum ip_conntrack_dir dir;
	const char *pattern;
	size_t plen;
	char skip;
	char term;
	enum ip_ct_ftp_type ftptype;
	int (*getnum)(const char *, size_t, u_int32_t[], char);
} search[] = {
	{
		IP_CT_DIR_ORIGINAL,
		"PORT",	sizeof("PORT") - 1, ' ', '\r',
		IP_CT_FTP_PORT,
		try_rfc959,
	},
	{
		IP_CT_DIR_REPLY,
		"227 ",	sizeof("227 ") - 1, '(', ')',
		IP_CT_FTP_PASV,
		try_rfc959,
	},
	{
		IP_CT_DIR_ORIGINAL,
		"EPRT", sizeof("EPRT") - 1, ' ', '\r',
		IP_CT_FTP_EPRT,
		try_eprt,
	},
	{
		IP_CT_DIR_REPLY,
		"229 ", sizeof("229 ") - 1, '(', ')',
		IP_CT_FTP_EPSV,
		try_epsv_response,
	},
};

static int try_number(const char *data, size_t dlen, u_int32_t array[],
		      int array_size, char sep, char term)
{
	u_int32_t i, len;

	memset(array, 0, sizeof(array[0])*array_size);

	/* Keep data pointing at next char. */
	for (i = 0, len = 0; len < dlen && i < array_size; len++, data++) {
		if (*data >= '0' && *data <= '9') {
			array[i] = array[i]*10 + *data - '0';
		}
		else if (*data == sep)
			i++;
		else {
			/* Unexpected character; true if it's the
			   terminator and we're finished. */
			if (*data == term && i == array_size - 1)
				return len;

			DEBUGP("Char %u (got %u nums) `%u' unexpected\n",
			       len, i, *data);
			return 0;
		}
	}
	DEBUGP("Failed to fill %u numbers separated by %c\n", array_size, sep);

	return 0;
}

/* Returns 0, or length of numbers: 192,168,1,1,5,6 */
static int try_rfc959(const char *data, size_t dlen, u_int32_t array[6],
		       char term)
{
	return try_number(data, dlen, array, 6, ',', term);
}

/* Grab port: number up to delimiter */
static int get_port(const char *data, int start, size_t dlen, char delim,
		    u_int32_t array[2])
{
	u_int16_t port = 0;
	int i;

	for (i = start; i < dlen; i++) {
		/* Finished? */
		if (data[i] == delim) {
			if (port == 0)
				break;
			array[0] = port >> 8;
			array[1] = port;
			return i + 1;
		}
		else if (data[i] >= '0' && data[i] <= '9')
			port = port*10 + data[i] - '0';
		else /* Some other crap */
			break;
	}
	return 0;
}

/* Returns 0, or length of numbers: |1|132.235.1.2|6275| */
static int try_eprt(const char *data, size_t dlen, u_int32_t array[6],
		    char term)
{
	char delim;
	int length;

	/* First character is delimiter, then "1" for IPv4, then
           delimiter again. */
	if (dlen <= 3) return 0;
	delim = data[0];
	if (isdigit(delim) || delim < 33 || delim > 126
	    || data[1] != '1' || data[2] != delim)
		return 0;

	DEBUGP("EPRT: Got |1|!\n");
	/* Now we have IP address. */
	length = try_number(data + 3, dlen - 3, array, 4, '.', delim);
	if (length == 0)
		return 0;

	DEBUGP("EPRT: Got IP address!\n");
	/* Start offset includes initial "|1|", and trailing delimiter */
	return get_port(data, 3 + length + 1, dlen, delim, array+4);
}

/* Returns 0, or length of numbers: |||6446| */
static int try_epsv_response(const char *data, size_t dlen, u_int32_t array[6],
			     char term)
{
	char delim;

	/* Three delimiters. */
	if (dlen <= 3) return 0;
	delim = data[0];
	if (isdigit(delim) || delim < 33 || delim > 126
	    || data[1] != delim || data[2] != delim)
		return 0;

	return get_port(data, 3, dlen, delim, array+4);
}

/* Return 1 for match, 0 for accept, -1 for partial. */
static int find_pattern(const char *data, size_t dlen,
			const char *pattern, size_t plen,
			char skip, char term,
			unsigned int *numoff,
			unsigned int *numlen,
			u_int32_t array[6],
			int (*getnum)(const char *, size_t, u_int32_t[], char))
{
	size_t i;

	DEBUGP("find_pattern `%s': dlen = %u\n", pattern, dlen);
	if (dlen == 0)
		return 0;

	if (dlen <= plen) {
		/* Short packet: try for partial? */
		if (strnicmp(data, pattern, dlen) == 0)
			return -1;
		else return 0;
	}

	if (strnicmp(data, pattern, plen) != 0) {
#if 0
		size_t i;

		DEBUGP("ftp: string mismatch\n");
		for (i = 0; i < plen; i++) {
			DEBUGP("ftp:char %u `%c'(%u) vs `%c'(%u)\n",
				i, data[i], data[i],
				pattern[i], pattern[i]);
		}
#endif
		return 0;
	}

	DEBUGP("Pattern matches!\n");
	/* Now we've found the constant string, try to skip
	   to the 'skip' character */
	for (i = plen; data[i] != skip; i++)
		if (i == dlen - 1) return -1;

	/* Skip over the last character */
	i++;

	DEBUGP("Skipped up to `%c'!\n", skip);

	*numoff = i;
	*numlen = getnum(data + i, dlen - i, array, term);
	if (!*numlen)
		return -1;

	DEBUGP("Match succeeded!\n");
	return 1;
}

/* FIXME: This should be in userspace.  Later. */
static int help(const struct iphdr *iph, size_t len,
		struct ip_conntrack *ct,
		enum ip_conntrack_info ctinfo)
{
	/* tcplen not negative guaranteed by ip_conntrack_tcp.c */
	struct tcphdr *tcph = (void *)iph + iph->ihl * 4;
	const char *data = (const char *)tcph + tcph->doff * 4;
	unsigned int tcplen = len - iph->ihl * 4;
	unsigned int datalen = tcplen - tcph->doff * 4;
	u_int32_t old_seq_aft_nl;
	int old_seq_aft_nl_set;
	u_int32_t array[6] = { 0 };
	int dir = CTINFO2DIR(ctinfo);
	unsigned int matchlen, matchoff;
	struct ip_ct_ftp_master *ct_ftp_info = &ct->help.ct_ftp_info;
	struct ip_conntrack_expect expect, *exp = &expect;
	struct ip_ct_ftp_expect *exp_ftp_info = &exp->help.exp_ftp_info;

	unsigned int i;
	int found = 0;

#ifdef CONFIG_IP_NF_ALGCTRL
	if (!ip_ct_alg_ftp) {
		return NF_ACCEPT;
	}
#endif
	
	/* Until there's been traffic both ways, don't look in packets. */
	if (ctinfo != IP_CT_ESTABLISHED
	    && ctinfo != IP_CT_ESTABLISHED+IP_CT_IS_REPLY) {
		DEBUGP("ftp: Conntrackinfo = %u\n", ctinfo);
		return NF_ACCEPT;
	}

	/* Not whole TCP header? */
	if (tcplen < sizeof(struct tcphdr) || tcplen < tcph->doff*4) {
		DEBUGP("ftp: tcplen = %u\n", (unsigned)tcplen);
		return NF_ACCEPT;
	}

	/* Checksum invalid?  Ignore. */
	/* FIXME: Source route IP option packets --RR */
	if (tcp_v4_check(tcph, tcplen, iph->saddr, iph->daddr,
			 csum_partial((char *)tcph, tcplen, 0))) {
		DEBUGP("ftp_help: bad csum: %p %u %u.%u.%u.%u %u.%u.%u.%u\n",
		       tcph, tcplen, NIPQUAD(iph->saddr),
		       NIPQUAD(iph->daddr));
		return NF_ACCEPT;
	}

	LOCK_BH(&ip_ftp_lock);
	old_seq_aft_nl_set = ct_ftp_info->seq_aft_nl_set[dir];
	old_seq_aft_nl = ct_ftp_info->seq_aft_nl[dir];

	DEBUGP("conntrack_ftp: datalen %u\n", datalen);
	if ((datalen > 0) && (data[datalen-1] == '\n')) {
		DEBUGP("conntrack_ftp: datalen %u ends in \\n\n", datalen);
		if (!old_seq_aft_nl_set
		    || after(ntohl(tcph->seq) + datalen, old_seq_aft_nl)) {
			DEBUGP("conntrack_ftp: updating nl to %u\n",
			       ntohl(tcph->seq) + datalen);
			ct_ftp_info->seq_aft_nl[dir] = 
						ntohl(tcph->seq) + datalen;
			ct_ftp_info->seq_aft_nl_set[dir] = 1;
		}
	}
	UNLOCK_BH(&ip_ftp_lock);

	if(!old_seq_aft_nl_set ||
			(ntohl(tcph->seq) != old_seq_aft_nl)) {
		DEBUGP("ip_conntrack_ftp_help: wrong seq pos %s(%u)\n",
		       old_seq_aft_nl_set ? "":"(UNSET) ", old_seq_aft_nl);
		return NF_ACCEPT;
	}

	/* Initialize IP array to expected address (it's not mentioned
           in EPSV responses) */
	array[0] = (ntohl(ct->tuplehash[dir].tuple.src.ip) >> 24) & 0xFF;
	array[1] = (ntohl(ct->tuplehash[dir].tuple.src.ip) >> 16) & 0xFF;
	array[2] = (ntohl(ct->tuplehash[dir].tuple.src.ip) >> 8) & 0xFF;
	array[3] = ntohl(ct->tuplehash[dir].tuple.src.ip) & 0xFF;

	for (i = 0; i < sizeof(search) / sizeof(search[0]); i++) {
		if (search[i].dir != dir) continue;

		found = find_pattern(data, datalen,
				     search[i].pattern,
				     search[i].plen,
				     search[i].skip,
				     search[i].term,
				     &matchoff, &matchlen,
				     array,
				     search[i].getnum);
		if (found) break;
	}
	if (found == -1) {
		/* We don't usually drop packets.  After all, this is
		   connection tracking, not packet filtering.
		   However, it is neccessary for accurate tracking in
		   this case. */
		if (net_ratelimit())
			printk("conntrack_ftp: partial %s %u+%u\n",
			       search[i].pattern,
			       ntohl(tcph->seq), datalen);
		return NF_DROP;
	} else if (found == 0) /* No match */
		return NF_ACCEPT;

	DEBUGP("conntrack_ftp: match `%.*s' (%u bytes at %u)\n",
	       (int)matchlen, data + matchoff,
	       matchlen, ntohl(tcph->seq) + matchoff);
	       
	memset(&expect, 0, sizeof(expect));

	/* Update the ftp info */
	LOCK_BH(&ip_ftp_lock);
	if (htonl((array[0] << 24) | (array[1] << 16) | (array[2] << 8) | array[3])
	    == ct->tuplehash[dir].tuple.src.ip) {
		exp->seq = ntohl(tcph->seq) + matchoff;
		exp_ftp_info->len = matchlen;
		exp_ftp_info->ftptype = search[i].ftptype;
		exp_ftp_info->port = array[4] << 8 | array[5];
#ifdef FAST_NAT_FTP_EFM_PATCH
		array[0] = (ntohl(ct->tuplehash[!dir].tuple.dst.ip) >> 24) & 0xFF;
		array[1] = (ntohl(ct->tuplehash[!dir].tuple.dst.ip) >> 16) & 0xFF;
		array[2] = (ntohl(ct->tuplehash[!dir].tuple.dst.ip) >> 8) & 0xFF;
		array[3] = ntohl(ct->tuplehash[!dir].tuple.dst.ip) & 0xFF;
#endif
	} else {
		/* Enrico Scholz's passive FTP to partially RNAT'd ftp
		   server: it really wants us to connect to a
		   different IP address.  Simply don't record it for
		   NAT. */
		DEBUGP("conntrack_ftp: NOT RECORDING: %u,%u,%u,%u != %u.%u.%u.%u\n",
		       array[0], array[1], array[2], array[3],
		       NIPQUAD(ct->tuplehash[dir].tuple.src.ip));

		/* Thanks to Cristiano Lincoln Mattos
		   <lincoln@cesar.org.br> for reporting this potential
		   problem (DMZ machines opening holes to internal
		   networks, or the packet filter itself). */
		if (!loose) goto out;
	}

	exp->tuple = ((struct ip_conntrack_tuple)
		{ { ct->tuplehash[!dir].tuple.src.ip,
		    { 0 } },
		  { htonl((array[0] << 24) | (array[1] << 16)
			  | (array[2] << 8) | array[3]),
		    { .tcp = { htons(array[4] << 8 | array[5]) } },
		    IPPROTO_TCP }});
	exp->mask = ((struct ip_conntrack_tuple)
		{ { 0xFFFFFFFF, { 0 } },
		  { 0xFFFFFFFF, { .tcp = { 0xFFFF } }, 0xFFFF }});

	exp->expectfn = NULL;

	/* Ignore failure; should only happen with NAT */
	ip_conntrack_expect_related(ct, &expect);
 out:
	UNLOCK_BH(&ip_ftp_lock);

	return NF_ACCEPT;
}

static struct ip_conntrack_helper ftp[MAX_PORTS];
static char ftp_names[MAX_PORTS][10];

#if defined(CONFIG_PROC_FS)
#ifdef FTP_CONNTRACK_NAT_UPDATE

static int ftpalg_read_proc(char *buffer, char **start, off_t off, int length, int *eof, void *data)
{
      char *p = buffer;
      int len, i;

      for (i=0; i < ports_c; i++)
                p += sprintf(p, "%d ", ports[i]);
      p += sprintf(p, "\n");

      len = p - buffer;
      if (len <= off+length) *eof = 1;
      *start = buffer + off;
      len -= off;
      if (len>length) len = length;
      if (len<0) len = 0;

      return len;

}

extern int ftp_alg_update_conntrack(int idx, unsigned short ftp_port_user, int action);
extern int ftp_alg_update_nat(int idx, unsigned short ftp_port_user, int action);

static int get_ports(const char *string, char *ftp_ports[])
{
        char *p;
        int n;

        ftp_ports[0]=0;
        n = 0;

        p = (char *) string;
        while (*p == ' ') p++;

        while (*p)
        {
                ftp_ports[n] = p;
                while (*p != ' ' && *p) p++;
                if (0==*p) goto out;
                *p++ = '\0';
                while (*p == ' ' && *p) p++;
out:
                n++;
                if (n == (MAX_PORTS-1))
                break;
        }
        return n;
}

static int ftpalg_write_proc(struct file *file, const char *buffer, unsigned long count, void *data)
{
	char tmpbuf[300];
	char *ftp_ports[MAX_PORTS];
	int ports_count, tmp_port, i;

	if (count < 2) return -EFAULT;

	if (buffer && !copy_from_user(&tmpbuf, buffer, count)) 
	{
		int n = ports_c;
		tmpbuf[count-1]='\0';

		// unregister user ftp private ports
		for (i = 1; i <n; i++)  // index 0 == default port 21
		{
			if (ports[i])
			{
				ftp_alg_update_conntrack(i, 0, 2);
				ftp_alg_update_nat(i, 0, 2);
			}
		}

		ports_count = get_ports((const char *)tmpbuf, ftp_ports);

		// register user ftp private ports
		for (i = 1; i <= ports_count; i++)  // index 0 == default port 21
		{
			sscanf(ftp_ports[i-1], "%d", &tmp_port);
			if (tmp_port)
			{
				ftp_alg_update_conntrack(i, (unsigned short)tmp_port, 1);
				ftp_alg_update_nat(i, (unsigned short)tmp_port, 1);
			}
		}

		return count;
      }
      return -EFAULT;
}

int ftp_alg_update_conntrack(int ftp_index, unsigned short ftp_port_user, int action)
{
	int ret;
	char *tmpname;

	if (action == 1)
	{
		ports[ftp_index] = ftp_port_user;
		ftp[ftp_index].tuple.src.u.tcp.port = htons(ftp_port_user);
		ftp[ftp_index].tuple.dst.protonum = IPPROTO_TCP;
		ftp[ftp_index].mask.src.u.tcp.port = 0xFFFF;
		ftp[ftp_index].mask.dst.protonum = 0xFFFF;
		ftp[ftp_index].max_expected = 1;
		ftp[ftp_index].timeout = 0;
		ftp[ftp_index].flags = IP_CT_HELPER_F_REUSE_EXPECT;
		ftp[ftp_index].me = ip_conntrack_ftp;
		ftp[ftp_index].help = help;
		tmpname = &ftp_names[ftp_index][0];
		sprintf(tmpname, "ftp-%d", ftp_port_user);
		ftp[ftp_index].name = tmpname;
			
		ret = ip_conntrack_helper_register(&ftp[ftp_index]);
	
		if (ret) {
			printk("ip_ct_ftp: error registering " "helper for port %d\n", ftp_port_user);
			ip_conntrack_helper_unregister(&ftp[ftp_index]);
			return ret;
		}
		ports_c++;

	} else if (action == 2)
	{
		ip_conntrack_helper_unregister(&ftp[ftp_index]);
		ports[ftp_index] = 0;
		ports_c--;
	}
	return 0;
}
#endif //FTP_CONNTRACK_NAT_UPDATE
#endif //CONFIG_PROC_FS


/* Not __exit: called from init() */
static void fini(void)
{
	int i;
	for (i = 0; i < ports_c; i++) {
		DEBUGP("ip_ct_ftp: unregistering helper for port %d\n",
				ports[i]);
		ip_conntrack_helper_unregister(&ftp[i]);
	}
#if defined(CONFIG_PROC_FS)
#ifdef  FTP_CONNTRACK_NAT_UPDATE
      if (resb) {
		remove_proc_entry("driver/ftp_port", resb);
		resb = NULL;
      }
#endif //FTP_CONNTRACK_NAT_UPDATE
#endif //CONFIG_PROC_FS
}

#undef printk
static int __init init(void)
{
	int i, ret;
	char *tmpname;
#if defined(CONFIG_PROC_FS)	
#ifdef FTP_CONNTRACK_NAT_UPDATE
	resb = create_proc_entry("driver/ftp_port", 0, 0);
	if (resb) {
		resb->read_proc = ftpalg_read_proc;
		resb->write_proc = ftpalg_write_proc;
		printk("FTP proc OK....\n");
	}
	else
		printk("FTP proc failed....\n");
#endif//FTP_CONNTRACK_NAT_UPDATE
#endif//CONFIG_PROC_FS
	if (ports[0] == 0)
		ports[0] = FTP_PORT;

	for (i = 0; (i < MAX_PORTS) && ports[i]; i++) {
		ftp[i].tuple.src.u.tcp.port = htons(ports[i]);
		ftp[i].tuple.dst.protonum = IPPROTO_TCP;
		ftp[i].mask.src.u.tcp.port = 0xFFFF;
		ftp[i].mask.dst.protonum = 0xFFFF;
		ftp[i].max_expected = 1;
		ftp[i].timeout = 0;
		ftp[i].flags = IP_CT_HELPER_F_REUSE_EXPECT;
		ftp[i].me = ip_conntrack_ftp;
		ftp[i].help = help;

		tmpname = &ftp_names[i][0];
		if (ports[i] == FTP_PORT)
			sprintf(tmpname, "ftp");
		else
			sprintf(tmpname, "ftp-%d", ports[i]);
		ftp[i].name = tmpname;

		printk("ip_ct_ftp: registering helper for port %d\n", 
				ports[i]);
		ret = ip_conntrack_helper_register(&ftp[i]);

		if (ret) {
			fini();
			return ret;
		}
		ports_c++;
	}
		printk("ip_ct_ftp: # of port %d\n",  ports_c);
	return 0;
}

EXPORT_SYMBOL(ip_ftp_lock);

MODULE_LICENSE("GPL");
module_init(init);
module_exit(fini);
