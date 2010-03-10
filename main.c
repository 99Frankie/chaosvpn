#include <sys/param.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "string/string.h"
#include "httplib/httplib.h"
#include "fs.h"
#include "list.h"
#include "main.h"
#include "tun.h"
#include "parser.h"
#include "tinc.h"
#include "settings.h"
#include "config.h"
#include "daemon.h"
#include "crypto.h"
#include "ar.h"
#include "uncompress.h"
#include "version.h"

static int r_sigterm = 0;
static int r_sigint = 0;
static struct daemon_info di_tincd;

extern FILE *yyin;

/* getopt switches */
static char* CONFIG_FILE = "/etc/tinc/chaosvpn.conf";
static int DONOTFORK = 0;

static time_t nextupdate = 0;
static struct string HTTP_USER_AGENT;


static int main_check_root(void);
static int main_create_backup(struct config*);
static int main_cleanup_hosts_subdir(struct config*);
static int main_fetch_and_apply_config(struct config* config, struct string* oldconfig);
static void main_free_parsed_info(struct config*);
static int main_init(struct config*);
static int main_load_previous_config(struct string*);
static int main_parse_config(struct config*, struct string*);
static void main_parse_opts(int, char**);
static int main_request_config(struct config*, struct string*);
static void main_tempsave_fetched_config(struct string*);
static void main_terminate_old_tincd(struct config*);
static void main_unlink_pidfile(void);
static void main_updated(void);
static void sigchild(int);
static void sigterm(int);
static void sigint(int);
static void sigint_holdon(int);
static void usage(void);

int
main (int argc,char *argv[])
{
	struct config *config;
	char tincd_debugparam[32];
	int err;
	struct string oldconfig;

	string_init(&HTTP_USER_AGENT, 64, 16);
	string_concat_sprintf(&HTTP_USER_AGENT, "ChaosVPN/%s", VERSION);

	printf("ChaosVPN/AgoraLink client v%s starting.\n", VERSION);

	main_parse_opts(argc, argv);

	config = config_alloc();
	if (config == NULL) {
		fprintf(stderr, "config malloc error\n");
		exit(EXIT_FAILURE);
	}

	if (main_check_root()) {
		fprintf(stderr, "Error - wrong user - please start as root user\n");
		return 1;
	}

#ifndef BSD
	if (tun_check_or_create()) {
		fprintf(stderr, "Error - unable to create tun device\n");
		return 1;
	}
#endif

	err = main_init(config);
	if (err) return err;

	config->tincd_version = tinc_get_version(config);
	if (config->tincd_version == NULL) {
		fprintf(stderr, "Warning: cant determine tinc version!\n");
	}

	string_init(&oldconfig, 4096, 4096);
	main_fetch_and_apply_config(config, &oldconfig);

	snprintf(tincd_debugparam, sizeof(tincd_debugparam), "--debug=%u", s_tincd_debuglevel);
	
	if (DONOTFORK) {
		daemon_init(&di_tincd, config->tincd_bin, config->tincd_bin, "-n", config->networkname, tincd_debugparam, NULL);
	} else {
		daemon_init(&di_tincd, config->tincd_bin, config->tincd_bin, "-n", config->networkname, tincd_debugparam, "-D", NULL);
		(void)signal(SIGTERM, sigterm);
		(void)signal(SIGINT, sigint);
		(void)signal(SIGCHLD, sigchild);
	}
	if (DONOTFORK) {
		main_terminate_old_tincd(config);
	} else {
		main_unlink_pidfile();
	}

	main_updated();

	puts("\x1B[31;1mStarting tincd.\x1B[0m");
	if (daemon_start(&di_tincd)) {
		(void)fputs("\x1B[31;1merror: unable to run tincd.\x1B[0m\n", stderr);
		exit(EXIT_FAILURE);
	}

	if (!DONOTFORK) {
		do {
		ml_cont:
			main_updated();
			while (1) {
				(void)sleep(2);
				if (nextupdate < time(NULL)) {
					break;
				}
				if (r_sigterm || r_sigint) {
					goto bail_out;
				}
			}
			switch (main_fetch_and_apply_config(config, &oldconfig)) {
			case -1:
				(void)fputs("\x1B[31mError while updating config. Not terminating tincd.\x1B[0m\n", stderr);
				goto ml_cont;

			case 1:
				(void)fputs("\x1B[31mNo update needed.\x1B[0m\n", stderr);
				goto ml_cont;

			default:;
			}
			puts("\x1B[31;1mTerminating tincd.\x1B[0m");
			daemon_stop(&di_tincd, 5);
		} while (!r_sigterm && !r_sigint);

		bail_out:
		puts("\x1B[31;1mTerminating tincd.\x1B[0m");
		(void)signal(SIGTERM, SIG_IGN);
		(void)signal(SIGINT, sigint_holdon);
		(void)signal(SIGCHLD, SIG_IGN);
		daemon_stop(&di_tincd, 5);
	}

	daemon_free(&di_tincd);
	settings_free_all();
	string_free(&oldconfig);
	free(config); config = NULL;

	return 0;
}

static void
main_updated(void)
{
    nextupdate = time(NULL) + s_update_interval;
}

static int
main_fetch_and_apply_config(struct config* config, struct string* oldconfig)
{
	int err;
	struct string http_response;

	(void)fputs("Fetching information:", stdout);
	(void)fflush(stdout);

	string_init(&http_response, 4096, 512);

	err = main_request_config(config, &http_response);
	if (err) {
		string_free(&http_response);
		if (main_load_previous_config(&http_response)) {
			return err;
		}
		(void)fputs("Warning: Unable to fetch config; using last stored config.", stderr);
	}

	if (string_equals(&http_response, oldconfig) == 0) {
		string_free(&http_response);
		return 1;
	}

	err = main_parse_config(config, &http_response);
	if (err) {
		string_free(&http_response);
		return -1;
	}

	// tempsave new config
	main_tempsave_fetched_config(&http_response);

	/* replaced by string_move */
	// string_free(&http_response);
	string_free(oldconfig);
	string_move(&http_response, oldconfig);

	(void)fputs(".\n", stdout);

	(void)fputs("Backing up old configs:", stdout);
	(void)fflush(stdout);
	if (main_create_backup(config)) {
		(void)fputs("Unable to complete config backup.\n", stderr);
		return -1;
	}
	(void)fputs(".\n", stdout);

	(void)fputs("Cleanup previous host entries:", stdout);
	(void)fflush(stdout);
	if (main_cleanup_hosts_subdir(config)) {
		(void)fputs("Unable to remove previous host subconfigs.\n", stderr);
		return -1;
	}
	(void)fputs(".\n", stdout);

	if (tinc_write_config(config)) return -1;
	if (tinc_write_hosts(config)) return -1;
	if (tinc_write_updown(config, true)) return -1;
	if (tinc_write_updown(config, false)) return -1;

	main_free_parsed_info(config);

	return 0;
}

static void
main_terminate_old_tincd(struct config *config)
{
	int pidfile;
	char pidbuf[32];
	int len;
	long readpid;
	pid_t pid;
	

	if (str_is_empty(config->pidfile))
		return;

	pidfile = open(config->pidfile, O_RDONLY);
	if (pidfile == -1) {
		(void)fprintf(stdout, "notice: unable to open pidfile '%s'; assuming an old tincd is not running\n", config->pidfile);
		return;
	}
	len = read(pidfile, pidbuf, 31);
	close(pidfile);
	pidbuf[len] = 0;
	readpid = strtol(pidbuf, NULL, 10);
	pid = (pid_t) readpid;
	(void)fprintf(stdout, "notice: sending SIGTERM to old tincd instance (%d).\n", pid);
	(void)kill(pid, SIGTERM);
	(void)sleep(2);
	if (kill(pid, SIGKILL) == 0) {
		(void)fputs("warning: tincd needed SIGKILL; unlinking its pidfile.\n", stderr);
		// SIGKILL succeeded; hence, we must manually unlink the old pidfile.
		main_unlink_pidfile();
	}
}

static void
main_parse_opts(int argc, char** argv)
{
	int c;

	opterr = 0;
	while ((c = getopt(argc, argv, "c:af")) != -1) {
		switch (c) {
		case 'c':
			CONFIG_FILE = optarg;
			break;

		case 'a':
			DONOTFORK = 1;
			break;

		case 'f':
			DONOTFORK = 0;
			break;

		default:
			usage();
		}
	}
}

static void
usage(void)
{
	(void)fputs("chaosvpn - connect to the chaos vpn.\n"
	       "Usage: chaosvpn [OPTION...]\n\n"
	       "  -c FILE  use this user configuration file\n"
	       "  -a       do not fork, onetime update and tincd restart\n"
	       "  -f       fork into background to be daemon, control tincd (default)\n"
	       "\n",
		stderr);
	exit(EXIT_FAILURE);
}

static int
main_check_root() {
	return getuid() != 0;
}

static int
main_init(struct config *config)
{
	struct stat st; 
	struct string privkey_name;

	yyin = fopen(CONFIG_FILE, "r");
	if (!yyin) {
		(void)fprintf(stderr, "Error: unable to open %s\n", CONFIG_FILE);
		return 1;
	}
	yyparse();
	fclose(yyin);

	if ((s_update_interval == 0) && (!DONOTFORK)) {
		(void)fputs("Error: you have not configured a remote config update interval.\n" \
					"($update_interval) Please configure an interval (3600 - 7200 seconds\n" \
					"are recommended) or activate legacy (cron) mode by using the -a flag.\n", stderr);
		exit(1);
	}
	if ((s_update_interval < 60) && (!DONOTFORK)) {
		(void)fputs("Error: $update_interval may not be <60.\n", stderr);
		exit(1);
	}


	// first copy all parsed params into config structure
	if (s_my_peerid != NULL)		config->peerid			= s_my_peerid;
	if (s_my_vpn_ip != NULL)		config->vpn_ip			= s_my_vpn_ip;
	if (s_my_vpn_ip6 != NULL)		config->vpn_ip6			= s_my_vpn_ip6;
	if (s_networkname != NULL)		config->networkname		= s_networkname;
	if (s_my_ip != NULL)			config->my_ip			= s_my_ip;
	if (s_tincd_bin != NULL)		config->tincd_bin		= s_tincd_bin;
	if (s_routeadd != NULL)			config->routeadd		= s_routeadd;
	if (s_routeadd6 != NULL)		config->routeadd6		= s_routeadd6;
	if (s_routedel != NULL)			config->routedel		= s_routedel;
	if (s_routedel6 != NULL)		config->routedel6		= s_routedel6;
	if (s_ifconfig != NULL)			config->ifconfig		= s_ifconfig;
	if (s_ifconfig6 != NULL)		config->ifconfig6		= s_ifconfig6;
	if (s_master_url != NULL)		config->master_url		= s_master_url;
	if (s_base != NULL)			config->base_path		= s_base;
	if (s_masterdata_signkey != NULL)	config->masterdata_signkey	= s_masterdata_signkey;
	if (s_tincd_graphdumpfile != NULL)	config->tincd_graphdumpfile	= s_tincd_graphdumpfile;
	if (s_pidfile != NULL)			config->pidfile			= s_pidfile;

	// then check required params
	#define reqparam(paramfield, label) if (str_is_empty(config->paramfield)) { \
		fprintf(stderr, "%s is missing or empty in %s\n", label, CONFIG_FILE); \
		return 1; \
		}

	reqparam(peerid, "$my_peerid");
	reqparam(networkname, "$networkname");
	reqparam(vpn_ip, "$my_vpn_ip");
	reqparam(routeadd, "$routeadd");
	reqparam(routedel, "$routedel");
	reqparam(ifconfig, "$ifconfig");
	reqparam(base_path, "$base");


	// create base directory
	if (stat(config->base_path, &st) & fs_mkdir_p(config->base_path, 0700)) {
		fprintf(stderr, "error: unable to mkdir %s\n", config->base_path);
		return 1;
	}

	string_init(&privkey_name, 1024, 512);
	if (string_concat_sprintf(&privkey_name, "%s/rsa_key.priv", config->base_path)) { return 1; }

	string_free(&config->privkey); /* just to be sure */
	if (fs_read_file(&config->privkey, string_get(&privkey_name))) {
		fprintf(stderr, "error: can't read private rsa key at %s\n", string_get(&privkey_name));
		string_free(&privkey_name);
		return 1;
	}
	string_free(&privkey_name);

	return 0;
}

static int
main_request_config(struct config *config, struct string *http_response)
{
	int retval = 1;
	int httpres;
	struct string httpurl;
	struct string archive;
	struct string chaosvpn_version;
	struct string signature;
	struct string compressed;
	struct string encrypted;
	struct string rsa_decrypted;
	struct string aes_key;
	struct string aes_iv;
	char *buf;
	time_t startfetchtime;

	startfetchtime = time(NULL);

	/* fetch main configfile */

	crypto_init();

	/* basic string inits first, makes for way easier error-cleanup */
	string_init(&httpurl, 512, 128);
	string_init(&archive, 8192, 8192);
	string_lazyinit(&chaosvpn_version, 16);
	string_lazyinit(&signature, 1024);
	string_lazyinit(&compressed, 8192);
	string_lazyinit(&encrypted, 8192);
	string_lazyinit(&rsa_decrypted, 1024);
	string_lazyinit(&aes_key, 64);
	string_lazyinit(&aes_iv, 64);

	//(void)fputs("Fetching information:", stdout);
	//void)fflush(stdout);
	string_concat_sprintf(&httpurl, "%s?id=%s",
		config->master_url, config->peerid);

	if ((retval = http_get(&httpurl, &archive, config->ifmodifiedsince, &HTTP_USER_AGENT, &httpres, NULL))) {
		if (retval == HTTP_ESRVERR) {
			if (httpres == 304) {
				fprintf(stderr, "Not fetching %s - got HTTP %d - not modified\n", config->master_url, httpres);
				retval = 1;
			} else {
				fprintf(stderr, "Unable to fetch %s - got HTTP %d\n", config->master_url, httpres);
			}
		} else if (retval == HTTP_EINVURL) {
			fprintf(stderr, "\x1B[41;37;1mInvalid URL %s. Only http:// is supported.\x1B[0m\n", config->master_url);
			exit(1);
		} else {
			fprintf(stderr, "Unable to fetch %s - maybe server is down\n", config->master_url);
		}
		goto bail_out;
	}
	config->ifmodifiedsince = startfetchtime;


	/* check if we received a new-style ar archive */
	if (!ar_is_ar_file(&archive)) {
		/* if we do not expect a signature than we can still use it
		   as the raw config (for debugging) */

		if (str_is_empty(config->masterdata_signkey)) {
			/* move whole received data to http_response */
			/* free old contents from http_response first */
			string_free(http_response);
			string_move(&archive, http_response);

			retval = 0; /* no error */
		} else {
			fprintf(stderr, "Invalid data format received from %s\n", config->master_url);
		}
		goto bail_out;
	}


	/* check chaosvpn-version in received ar archive */
	if (ar_extract(&archive, "chaosvpn-version", &chaosvpn_version)) {
		string_free(&chaosvpn_version);
		fprintf(stderr, "chaosvpn-version missing - cant work with this config\n");
		goto bail_out;
	}
	string_putc(&chaosvpn_version, '\0');
	if (strcmp(string_get(&chaosvpn_version), "3") != 0) {
		string_free(&chaosvpn_version);
		fprintf(stderr, "unusable data-version from backend, we only support version 3!\n");
		goto bail_out;
	}
	string_free(&chaosvpn_version);

	if (str_is_empty(config->masterdata_signkey)) {
		/* no public key defined, nothing to verify against or to decrypt with */
		/* expect cleartext part */

		if (ar_extract(&archive, "cleartext", http_response)) {
			fprintf(stderr, "cleartext part missing - cant work with this config\n");
			goto bail_out;
		}

		/* return success */
		retval = 0;
		goto bail_out;
	}


	/* get and decrypt rsa data block */
	if (ar_extract(&archive, "rsa", &encrypted)) {
		fprintf(stderr, "rsa part in data from %s missing\n", config->master_url);
		goto bail_out;
	}
	if (crypto_rsa_decrypt(&encrypted, string_get(&config->privkey), &rsa_decrypted)) {
		fprintf(stderr, "rsa decrypt failed\n");
		goto bail_out;
	}
	string_free(&encrypted);

	/* check and copy data decrypted from rsa block */
	/* structure:
		1 byte length aes key
		1 byte length aes iv
		x bytes aes key
		y bytes aes iv
	   */
	if (string_length(&rsa_decrypted) < 2) {
		fprintf(stderr, "rsa decrypt result too short\n");
		goto bail_out;
	}
	buf = string_get(&rsa_decrypted);
	if (buf[0] != 32) {
		fprintf(stderr, "invalid aes keysize - expected 32, received %d\n", buf[0]);
		goto bail_out;
	}
	if (buf[1] != 16) {
		fprintf(stderr, "invalid aes ivsize - expected 16, received %d\n", buf[1]);
		goto bail_out;
	}
	if (string_length(&rsa_decrypted) < buf[0]+buf[1]+2) {
		fprintf(stderr, "rsa decrypt result too short\n");
		goto bail_out;
	}
	string_concatb(&aes_key, buf+2, buf[0]);
	string_concatb(&aes_iv, buf+2+buf[0], buf[1]);

	/* get, decrypt and uncompress config data */
	if (ar_extract(&archive, "encrypted", &encrypted)) {
		fprintf(stderr, "encrypted data part in data from %s missing\n", config->master_url);
		goto bail_out;
	}
	if (crypto_aes_decrypt(&encrypted, &aes_key, &aes_iv, &compressed)) {
		fprintf(stderr, "data decrypt failed\n");
		goto bail_out;
	}
	string_free(&encrypted);
	if (uncompress_inflate(&compressed, http_response)) {
		fprintf(stderr, "data uncompress failed\n");
		goto bail_out;
	}
	string_free(&compressed);

	/* get and decrypt signature */
	if (ar_extract(&archive, "signature", &encrypted)) {
		fprintf(stderr, "signature part in data from %s missing\n", config->master_url);
		goto bail_out;
	}
	if (crypto_aes_decrypt(&encrypted, &aes_key, &aes_iv, &signature)) {
		fprintf(stderr, "signature decrypt failed\n");
		goto bail_out;
	}
	string_free(&encrypted);

	/* verify signature */
	retval = crypto_rsa_verify_signature(http_response, &signature, config->masterdata_signkey);


bail_out:
	/* free all strings, even if we may already freed them above */
	/* double string_free() is ok, and the error cleanup this way is easier */
	string_free(&httpurl);
	string_free(&archive);
	string_free(&chaosvpn_version);
	string_free(&signature);
	string_free(&compressed);
	string_free(&encrypted);
	string_free(&rsa_decrypted);
	string_free(&aes_key);
	string_free(&aes_iv);

	// make sure result is null-terminated
	// ar_extract() and crypto_*_decrypt() do not guarantee this!
	string_putc(http_response, '\0');

	crypto_finish();

	return retval;
}

static int
main_parse_config(struct config *config, struct string *http_response)
{
	struct list_head *p = NULL;

	if (parser_parse_config(string_get(http_response), &config->peer_config)) {
		fprintf(stderr, "\nUnable to parse config\n");
		return 1;
	}

	list_for_each(p, &config->peer_config) {
		struct peer_config_list *i = container_of(p,
				struct peer_config_list, list);
		if (strcmp(i->peer_config->name, config->peerid) == 0) {
				config->my_peer = i->peer_config;
		}
	}

	if (config->my_peer == NULL) {
		fprintf(stderr, "\nUnable to find %s in config.\n", config->peerid);
		return 1;
	}

	return 0;
}

static void
main_free_parsed_info(struct config* config)
{
	parser_free_config(&config->peer_config);
}

static void
main_tempsave_fetched_config(struct string* cnf)
{
	int fd;
    static int NOTMPFILEWARNED = 0;

	if (s_tmpconffile == NULL) {
        if (NOTMPFILEWARNED) return;
		(void)fputs("Warning: not tempsaving fetched config. Set $tmpconffile\n"
				" in chaosvpn.conf to enable.", stderr);
        NOTMPFILEWARNED = 1;
		return;
	}

	fd = open(s_tmpconffile, O_WRONLY | O_CREAT, 0600);
	if (fd == -1) return;

	if (write(fd, string_get(cnf), string_length(cnf)) != string_length(cnf)) {
		(void)unlink(s_tmpconffile);
	}
	close(fd);
}

static int
main_load_previous_config(struct string* cnf)
{
	int fd;
	struct stat sb;
	intptr_t readbytes;
	int retval = 1;

	if (s_tmpconffile == NULL) return 1;

	fd = open(s_tmpconffile, O_RDONLY);
	if (fd == -1) return 1;

	if (fstat(fd, &sb)) return 1;
	if (string_read(cnf, fd, sb.st_size, &readbytes)) {
		(void)fputs("Error: not enough memory to read stored config file.\n", stderr);
		string_clear(cnf);
		goto bail_out;
	}

	if (readbytes != sb.st_size) {
		(void)fputs("Error: unable to fully read stored config file.\n", stderr);
		string_clear(cnf);
		goto bail_out;
	}

	retval = 0;
bail_out:
	close(fd);
	return retval;
}

static int
main_create_backup(struct config *config)
{
	int retval = 1;
	struct string base_backup_fn;

	if (string_init(&base_backup_fn, 512, 512)) return 1; /* don't goto bail_out here */
	if (string_concat(&base_backup_fn, config->base_path)) goto bail_out;
	if (string_concatb(&base_backup_fn, ".old", 5)) goto bail_out;

	retval = fs_cp_r(config->base_path, string_get(&base_backup_fn));

	/* fall through */
bail_out:
	string_free(&base_backup_fn);
	return retval;
}

static int
main_cleanup_hosts_subdir(struct config *config)
{
	int retval = 1;
	struct string hosts_dir;
	
	if (string_init(&hosts_dir, 512, 512)) return 1; /* don't goto bail_out here */
	if (string_concat(&hosts_dir, config->base_path)) goto bail_out;
	if (string_concat(&hosts_dir, "/hosts")) goto bail_out;

	retval = fs_empty_dir(string_get(&hosts_dir));

	/* fall through */
bail_out:
	string_free(&hosts_dir);
	return retval;
}

static void
main_unlink_pidfile(void)
{
	(void)unlink(s_pidfile);
}

static void
sigchild(int sig /*__unused*/)
{
	fprintf(stderr, "\x1B[31;1mtincd terminated. Restarting in %d seconds.\x1B[0m\n", s_tincd_restart_delay);
	main_unlink_pidfile();
	if (daemon_sigchld(&di_tincd, s_tincd_restart_delay)) {
		fputs("\x1B[31;1munable to restart tincd. Terminating.\x1B[0m\n", stderr);
		exit(EXIT_FAILURE);
	}
}

static void
sigterm(int sig /*__unused*/)
{
	r_sigterm = 1;
}

static void
sigint(int sig /*__unused*/)
{
	r_sigint = 1;
}

static void
sigint_holdon(int sig /*__unused*/)
{
	puts("I'm doing me best, please be patient for a little, will ya?");
}

