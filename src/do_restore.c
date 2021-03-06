#include "destor.h"
#include "jcr.h"
#include "recipe/recipestore.h"
#include "storage/containerstore.h"
#include "utils/lru_cache.h"
#include "restore.h"

extern void init_segmenting_method();

static pthread_cond_t cond;  
static pthread_mutex_t mutex;  

/*********************fdl********************************************
static pthread_t thread_wait; 
static pthread_cond_t cond;  
static pthread_mutex_t mutex;  
static int flag = 1;

void * thr_wait(void *arg)
{
    struct timeval now;
    struct timespec outtime;
    pthread_mutex_lock(&mutex);
    while(jcr.status == JCR_STATUS_RUNNING || jcr.status != JCR_STATUS_DONE){
        gettimeofday(&now, NULL);  
        outtime.tv_sec = now.tv_sec + 5;  
        outtime.tv_nsec = now.tv_usec * 1000;  
        pthread_cond_timedwait(&cond, &mutex, &outtime); 
    }
    pthread_mutex_unlock(&mutex); 
}



*********************fdl********************************************/
static void* lru_restore_thread(void *arg) {
	struct lruCache *cache;
	if (destor.simulation_level >= SIMULATION_RESTORE)
		cache = new_lru_cache(destor.restore_cache[1], (void*)free_container_meta,
				lookup_fingerprint_in_container_meta);
	else
		cache = new_lru_cache(destor.restore_cache[1], (void*)free_container,
				lookup_fingerprint_in_container);

	struct chunk* c;
	while ((c = sync_queue_pop(restore_recipe_queue))) {

		if (CHECK_CHUNK(c, CHUNK_FILE_START) || CHECK_CHUNK(c, CHUNK_FILE_END)) {
			sync_queue_push(restore_chunk_queue, c);
			continue;
		}

		TIMER_DECLARE(1);
		TIMER_BEGIN(1);

		if (destor.simulation_level >= SIMULATION_RESTORE) {
			struct containerMeta *cm = lru_cache_lookup(cache, &c->fp);
			if (!cm) {
				VERBOSE("Restore cache: container %lld is missed", c->id);
				cm = retrieve_container_meta_by_id(c->id);
				assert(lookup_fingerprint_in_container_meta(cm, &c->fp));
				lru_cache_insert(cache, cm, NULL, NULL);
				jcr.read_container_num++;
			}

			TIMER_END(1, jcr.read_chunk_time);
		} else {
			struct container *con = lru_cache_lookup(cache, &c->fp);
			if (!con) {
				VERBOSE("Restore cache: container %lld is missed", c->id);
				con = retrieve_container_by_id(c->id);
				lru_cache_insert(cache, con, NULL, NULL);
				jcr.read_container_num++;
			}
			struct chunk *rc = get_chunk_in_container(con, &c->fp);
			assert(rc);
			TIMER_END(1, jcr.read_chunk_time);
			sync_queue_push(restore_chunk_queue, rc);
		}

		jcr.data_size += c->size;
		jcr.chunk_num++;
		free_chunk(c);
	}

	sync_queue_term(restore_chunk_queue);

	free_lru_cache(cache);

	return NULL;
}

static void* read_recipe_thread(void *arg) {

	int i, j, k;
	for (i = 0; i < jcr.bv->number_of_files; i++) {
		TIMER_DECLARE(1);
		TIMER_BEGIN(1);

		struct fileRecipeMeta *r = read_next_file_recipe_meta(jcr.bv);

		struct chunk *c = new_chunk(sdslen(r->filename) + 1);
		strcpy(c->data, r->filename);
		SET_CHUNK(c, CHUNK_FILE_START);

		TIMER_END(1, jcr.read_recipe_time);

		sync_queue_push(restore_recipe_queue, c);

		for (j = 0; j < r->chunknum; j++) {
			TIMER_DECLARE(1);
			TIMER_BEGIN(1);

			struct chunkPointer* cp = read_next_n_chunk_pointers(jcr.bv, 1, &k);

			struct chunk* c = new_chunk(0);
			memcpy(&c->fp, &cp->fp, sizeof(fingerprint));
			c->size = cp->size;
			c->id = cp->id;

			TIMER_END(1, jcr.read_recipe_time);

			sync_queue_push(restore_recipe_queue, c);
			free(cp);
		}

		c = new_chunk(0);
		SET_CHUNK(c, CHUNK_FILE_END);
		sync_queue_push(restore_recipe_queue, c);

		free_file_recipe_meta(r);
	}

	sync_queue_term(restore_recipe_queue);
	return NULL;
}

void* write_restore_data(void* arg) {

	char *p, *q;
	q = jcr.path + 1;/* ignore the first char*/
	/*
	 * recursively make directory
	 */
	while ((p = strchr(q, '/'))) {
		if (*p == *(p - 1)) {
			q++;
			continue;
		}
		*p = 0;
		if (access(jcr.path, 0) != 0) {
			mkdir(jcr.path, S_IRWXU | S_IRWXG | S_IRWXO);
		}
		*p = '/';
		q = p + 1;
	}

	struct chunk *c = NULL;
	FILE *fp = NULL;

	while ((c = sync_queue_pop(restore_chunk_queue))) {

		TIMER_DECLARE(1);
		TIMER_BEGIN(1);

		if (CHECK_CHUNK(c, CHUNK_FILE_START)) {
			VERBOSE("Restoring: %s", c->data);

			sds filepath = sdsdup(jcr.path);
			filepath = sdscat(filepath, c->data);

			int len = sdslen(jcr.path);
			char *q = filepath + len;
			char *p;
			while ((p = strchr(q, '/'))) {
				if (*p == *(p - 1)) {
					q++;
					continue;
				}
				*p = 0;
				if (access(filepath, 0) != 0) {
					mkdir(filepath, S_IRWXU | S_IRWXG | S_IRWXO);
				}
				*p = '/';
				q = p + 1;
			}

			if (destor.simulation_level == SIMULATION_NO) {
				assert(fp == NULL);
				fp = fopen(filepath, "w");
			}

			sdsfree(filepath);

		} else if (CHECK_CHUNK(c, CHUNK_FILE_END)) {
		    jcr.file_num++;

			if (fp)
				fclose(fp);
			fp = NULL;
		} else {
			assert(destor.simulation_level == SIMULATION_NO);
			VERBOSE("Restoring %d bytes", c->size);
			fwrite(c->data, c->size, 1, fp);
		}

		free_chunk(c);

		TIMER_END(1, jcr.write_chunk_time);
	}

    jcr.status = JCR_STATUS_DONE;
    pthread_cond_signal(&cond);
    return NULL;
}

void do_restore(int revision, char *path) {
	init_recipe_store();
	init_container_store();
    
    pthread_mutex_init(&mutex, NULL);  
    pthread_cond_init(&cond, NULL);  

    init_segmenting_method();

	init_restore_jcr(revision, path);

	destor_log(DESTOR_NOTICE, "job id: %d", jcr.id);
	destor_log(DESTOR_NOTICE, "backup path: %s", jcr.bv->path);
	destor_log(DESTOR_NOTICE, "restore to: %s", jcr.path);

	restore_chunk_queue = sync_queue_new(1024);
	restore_recipe_queue = sync_queue_new(1024);

	TIMER_DECLARE(1);
	TIMER_BEGIN(1);

	

    jcr.status = JCR_STATUS_RUNNING;
	pthread_t recipe_t, read_t, write_t;
	pthread_create(&recipe_t, NULL, read_recipe_thread, NULL);

	if (destor.restore_cache[0] == RESTORE_CACHE_LRU) {
		destor_log(DESTOR_NOTICE, "restore cache is LRU");
		pthread_create(&read_t, NULL, lru_restore_thread, NULL);
	} else if (destor.restore_cache[0] == RESTORE_CACHE_OPT) {
		destor_log(DESTOR_NOTICE, "restore cache is OPT");
		pthread_create(&read_t, NULL, optimal_restore_thread, NULL);
	} else if (destor.restore_cache[0] == RESTORE_CACHE_ASM) {
		destor_log(DESTOR_NOTICE, "restore cache is ASM");
		pthread_create(&read_t, NULL, assembly_restore_thread, NULL);
    } else if (destor.restore_cache[0] == RESTORE_CACHE_PATTERN) {
        destor_log(DESTOR_NOTICE, "restore cache is PATTERN");
        pthread_create(&read_t, NULL, pattern_restore_plus_thread, NULL);
    } else {
		fprintf(stderr, "Invalid restore cache.\n");
		exit(1);
	}

	pthread_create(&write_t, NULL, write_restore_data, NULL);

    //do{
        ;//sleep(0.1);
        //time_t now = time(NULL);
        //fprintf(stderr, "%" PRId64 " bytes, %" PRId32 " chunks, %d files processed\r", 
        //        jcr.data_size, jcr.chunk_num, jcr.file_num);
    //}while(jcr.status == JCR_STATUS_RUNNING || jcr.status != JCR_STATUS_DONE);
    
    
    struct timeval now;
    struct timespec outtime;
    pthread_mutex_lock(&mutex);
    while(jcr.status == JCR_STATUS_RUNNING || jcr.status != JCR_STATUS_DONE){
        fprintf(stderr, "%" PRId64 " bytes, %" PRId32 " chunks, %d files processed\r", 
                jcr.data_size, jcr.chunk_num, jcr.file_num);
        gettimeofday(&now, NULL);  
        outtime.tv_sec = now.tv_sec + 5;  
        outtime.tv_nsec = now.tv_usec * 1000;  
        pthread_cond_timedwait(&cond, &mutex, &outtime); 
    }
    pthread_mutex_unlock(&mutex); 
    /***********************************fdl*************************
    pthread_mutex_init(&mutex, NULL);  
    pthread_cond_init(&cond, NULL); 
    if (0 != pthread_create(&thread_wait, NULL, thr_wait, NULL)) {
        exit(1);
    }
    **********************************fdl**************************/
    fprintf(stderr, "%" PRId64 " bytes, %" PRId32 " chunks, %d files processed\n", 
        jcr.data_size, jcr.chunk_num, jcr.file_num);

	assert(sync_queue_size(restore_chunk_queue) == 0);
	assert(sync_queue_size(restore_recipe_queue) == 0);

	free_backup_version(jcr.bv);

	TIMER_END(1, jcr.total_time);
	

	printf("job id: %" PRId32 "\n", jcr.id);
	printf("restore path: %s\n", jcr.path);
	printf("number of files: %" PRId32 "\n", jcr.file_num);
	printf("number of chunks: %" PRId32"\n", jcr.chunk_num);
	printf("total size(B): %" PRId64 "\n", jcr.data_size);
	printf("total time(s): %.3f\n", jcr.total_time / 1000000);
	printf("throughput(MB/s): %.2f\n",
			jcr.data_size * 1000000 / (1024.0 * 1024 * jcr.total_time));
	printf("speed factor: %.2f\n",
			jcr.data_size / (1024.0 * 1024 * jcr.read_container_num));

	printf("read_recipe_time : %.3fs, %.2fMB/s\n",
			jcr.read_recipe_time / 1000000,
			jcr.data_size * 1000000 / jcr.read_recipe_time / 1024 / 1024);
	printf("read_chunk_time : %.3fs, %.2fMB/s\n", jcr.read_chunk_time / 1000000,
			jcr.data_size * 1000000 / jcr.read_chunk_time / 1024 / 1024);
	printf("write_chunk_time : %.3fs, %.2fMB/s\n",
			jcr.write_chunk_time / 1000000,
			jcr.data_size * 1000000 / jcr.write_chunk_time / 1024 / 1024);

	char logfile[] = "restore.log";
	FILE *fp = fopen(logfile, "a");

	/*
	 * job id,
	 * chunk num,
	 * data size,
	 * actually read container number,
	 * speed factor,
	 * throughput
	 */
	fprintf(fp, "%" PRId32 " %" PRId64 " %" PRId32 " %.4f %.4f\n", jcr.id, jcr.data_size,
			jcr.read_container_num,
			jcr.data_size / (1024.0 * 1024 * jcr.read_container_num),
			jcr.data_size * 1000000 / (1024 * 1024 * jcr.total_time));

	fclose(fp);

	close_container_store();
	close_recipe_store();
}

