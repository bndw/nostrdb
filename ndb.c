

#include "nostrdb.h"
#include "print_util.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

static int usage()
{
	printf("usage: ndb [--skip-verification] [-d db_dir] <command>\n\n");
	printf("commands\n\n");
	printf("	stat\n");
	printf("	search [--oldest-first] [--limit 42] <fulltext query>\n");
	printf("	query [-k 42] [-k 1337] [-l 42]\n");
	printf("	import <line-delimited json file>\n\n");
	printf("settings\n\n");
	printf("	--skip-verification  skip signature validation\n");
	printf("	-d <db_dir>          set database directory\n");
	return 1;
}


static int map_file(const char *filename, unsigned char **p, size_t *flen)
{
	struct stat st;
	int des;
	stat(filename, &st);
	*flen = st.st_size;

	des = open(filename, O_RDONLY);

	*p = mmap(NULL, *flen, PROT_READ, MAP_PRIVATE, des, 0);
	close(des);

	return *p != MAP_FAILED;
}

static inline void print_stat_counts(struct ndb_stat_counts *counts)
{
	printf("%zu\t%zu\t%zu\t%zu\n",
	       counts->count,
	       counts->key_size,
	       counts->value_size,
	       counts->key_size + counts->value_size);
}

static void print_stats(struct ndb_stat *stat)
{
	int i;
	const char *name;
	struct ndb_stat_counts *c;

	struct ndb_stat_counts total;
	ndb_stat_counts_init(&total);

	printf("name\tcount\tkey_bytes\tvalue_bytes\ttotal_bytes\n");
	printf("---\ndbs\n---\n");
	for (i = 0; i < NDB_DBS; i++) {
		name = ndb_db_name(i);

		total.count += stat->dbs[i].count;
		total.key_size += stat->dbs[i].key_size;
		total.value_size += stat->dbs[i].value_size;

		printf("%s\t", name);
		print_stat_counts(&stat->dbs[i]);
	}

	printf("total\t");
	print_stat_counts(&total);

	printf("-----\nkinds\n-----\n");
	for (i = 0; i < NDB_CKIND_COUNT; i++) {
		c = &stat->common_kinds[i];
		if (c->count == 0)
			continue;

		printf("%s\t", ndb_kind_name(i));
		print_stat_counts(c);
	}

	if (stat->other_kinds.count != 0) {
		printf("other\t");
		print_stat_counts(&stat->other_kinds);
	}
}

int ndb_print_search_keys(struct ndb_txn *txn);
int ndb_print_kind_keys(struct ndb_txn *txn);
int ndb_print_tag_keys(struct ndb_txn *txn);

static void print_note(struct ndb_note *note)
{
	printf("%d\t%d\t%s",
		ndb_note_kind(note),
		ndb_note_created_at(note),
		ndb_note_content(note));

	printf("\n");
}

int main(int argc, char *argv[])
{
	struct ndb *ndb;
	int i, flags, limit, count, current_field;
	long nanos;
	struct ndb_stat stat;
	struct ndb_txn txn;
	struct ndb_text_search_results results;
	struct ndb_text_search_result *result;
	const char *dir;
	unsigned char *data;
	size_t data_len;
	struct ndb_config config;
	struct timespec t1, t2;
	struct ndb_text_search_config search_config;
	ndb_default_config(&config);
	ndb_default_text_search_config(&search_config);
	ndb_config_set_mapsize(&config, 1024ULL * 1024ULL * 1024ULL * 1024ULL /* 1 TiB */);

	if (argc < 2) {
		return usage();
	}

	dir = ".";
	flags = 0;
	for (i = 0; i < 2; i++)
	{
		if (!strcmp(argv[1], "-d") && argv[2]) {
			dir = argv[2];
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[1], "--skip-verification")) {
			flags = NDB_FLAG_SKIP_NOTE_VERIFY;
			argv += 1;
			argc -= 1;
		}
	}

	ndb_config_set_flags(&config, flags);

	fprintf(stderr, "using db '%s'\n", dir);

	if (!ndb_init(&ndb, dir, &config)) {
		return 2;
	}

	if (argc >= 3 && !strcmp(argv[1], "search")) {
		for (i = 0; i < 2; i++) {
			if (!strcmp(argv[2], "--oldest-first")) {
				ndb_text_search_config_set_order(&search_config, NDB_ORDER_ASCENDING);
				argv++;
				argc--;
			} else if (!strcmp(argv[2], "--limit") || !strcmp(argv[2], "-l")) {
				limit = atoi(argv[3]);
				ndb_text_search_config_set_limit(&search_config, limit);
				argv += 2;
				argc -= 2;
			}
		}

		ndb_begin_query(ndb, &txn);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ndb_text_search(&txn, argv[2], &results, &search_config);
		clock_gettime(CLOCK_MONOTONIC, &t2);

		nanos = (t2.tv_sec - t1.tv_sec) * (long)1e9 + (t2.tv_nsec - t1.tv_nsec);

		fprintf(stderr, "%d results in %f ms\n", results.num_results, nanos/1000000.0);

		// print results for now
		for (i = 0; i < results.num_results; i++) {
			result = &results.results[i];
			printf("[%02d] ", i+1);
			ndb_print_text_search_result(&txn, result);
		}

		ndb_end_query(&txn);
	} else if (argc == 2 && !strcmp(argv[1], "stat")) {
		if (!ndb_stat(ndb, &stat)) {
			return 3;
		}

		print_stats(&stat);
	} else if (argc >= 3 && !strcmp(argv[1], "query")) {
		struct ndb_filter filter, *f = &filter;
		ndb_filter_init(f);

		argv += 2;
		argc -= 2;
		current_field = 0;

		for (i = 0; argc && i < 100; i++) {
			if (!strcmp(argv[0], "-k")) {
				if (current_field != NDB_FILTER_KINDS)
					ndb_filter_start_field(f, NDB_FILTER_KINDS);
				current_field = NDB_FILTER_KINDS;
				ndb_filter_add_int_element(f, atoll(argv[1]));
				argv += 2;
				argc -= 2;
			} else if (!strcmp(argv[0], "-l")) {
				limit = atol(argv[1]);
				if (current_field) {
					ndb_filter_end_field(f);
					current_field = 0;
				}
				ndb_filter_start_field(f, NDB_FILTER_LIMIT);
				current_field = NDB_FILTER_LIMIT;
				ndb_filter_add_int_element(f, limit);
				ndb_filter_end_field(f);
				argv += 2;
				argc -= 2;
			} else if (!strcmp(argv[0], "-u")) {
				if (current_field) {
					ndb_filter_end_field(f);
					current_field = 0;
				}
				ndb_filter_start_field(f, NDB_FILTER_UNTIL);
				ndb_filter_add_int_element(f, atoll(argv[1]));
				ndb_filter_end_field(f);
				argv += 2;
				argc -= 2;
			} else if (!strcmp(argv[0], "-t")) {
				if (current_field) {
					ndb_filter_end_field(f);
					current_field = 0;
				}
				ndb_filter_start_tag_field(f, 't');
				ndb_filter_add_str_element(f, argv[1]);
				ndb_filter_end_field(f);
				argv += 2;
				argc -= 2;
			}
		}

		if (current_field) {
			ndb_filter_end_field(f);
			current_field = 0;
		}

		struct ndb_query_result results[10000];
		ndb_begin_query(ndb, &txn);

		clock_gettime(CLOCK_MONOTONIC, &t1);
		ndb_query(&txn, f, 1, results, 10000, &count);
		clock_gettime(CLOCK_MONOTONIC, &t2);

		nanos = (t2.tv_sec - t1.tv_sec) * (long)1e9 + (t2.tv_nsec - t1.tv_nsec);

		fprintf(stderr, "%d results in %f ms\n", count, nanos/1000000.0);
		for (i = 0; i < count; i++) {
			print_note(results[i].note);
		}

		ndb_end_query(&txn);

	} else if (argc == 3 && !strcmp(argv[1], "import")) {
		if (!strcmp(argv[2], "-")) {
			ndb_process_events_stream(ndb, stdin);
		} else {
			map_file(argv[2], &data, &data_len);
			ndb_process_events(ndb, (const char *)data, data_len);
			ndb_process_client_events(ndb, (const char *)data, data_len);
		}
	} else if (argc == 2 && !strcmp(argv[1], "print-search-keys")) {
		ndb_begin_query(ndb, &txn);
		ndb_print_search_keys(&txn);
		ndb_end_query(&txn);
	} else if (argc == 2 && !strcmp(argv[1], "print-kind-keys")) {
		ndb_begin_query(ndb, &txn);
		ndb_print_kind_keys(&txn);
		ndb_end_query(&txn);
	} else if (argc == 2 && !strcmp(argv[1], "print-tag-keys")) {
		ndb_begin_query(ndb, &txn);
		ndb_print_tag_keys(&txn);
		ndb_end_query(&txn);
	} else {
		return usage();
	}

	ndb_destroy(ndb);
}
