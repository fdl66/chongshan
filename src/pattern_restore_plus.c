
#include "destor.h"
#include "jcr.h"
#include "recipe/recipestore.h"
#include "storage/containerstore.h"
#include "index/index.h"
#include "restore.h"
#include "utils/lru_cache.h"


struct pattern_chunk{
    struct metaEntry *me;
    unsigned char* data;
};
//to store all chunks of s1_chunk_list
static GHashTable *ht_pattern_chunks;

static struct lruCache *dataCache;
static struct lruCache *metaCache;

unsigned char* allocate_data_buffer(int *pattern, GList* b, int len){
    //allocate the buffer space
    int i;
    unsigned char* write_buf;
    int buf_len = 0;
    struct metaEntry *me;
    
    GList* b_iter = b;
    for (i=0; i<len; i++) {
        if (pattern[i]) {
            me = (struct metaEntry*)b_iter->data;
            buf_len += me->len;
        }
        b_iter = g_list_next(b_iter);
    }
    write_buf = (unsigned char*)malloc(buf_len);
    
    return write_buf;
}


static struct chunk* dup_chunk(struct chunk* ch){
    struct chunk *dup = malloc(sizeof(struct chunk));
    memcpy(dup, ch, sizeof(struct chunk));
    dup->data = malloc(ch->size);
    memcpy(dup->data, ch->data, ch->size);
    return dup;
}



static void remove_chunk_from_ht_data_cache(struct chunk *ch, GHashTable *ht){
    assert(ch);
    int t = g_hash_table_remove(ht, &ch->fp);
    assert(t);
}




static void read_data_by_pattern_in_data_cache(GSequence *a, int a_len, struct container *con){
    //assign data for each chunk in recipe list
    
    //DEBUG("read in container cache");
    
    int i;
    GSequenceIter *a_iter = g_sequence_get_begin_iter(a);
    for (i=0; i<a_len; i++) {
        struct chunk *ch = (struct chunk*)g_sequence_get(a_iter);
        
        if (ch->id == con->meta.id) {
            struct chunk *rc = get_chunk_in_container(con, &ch->fp);
            ch->data = rc->data;
            free(rc);
            
            GSequenceIter *t = g_sequence_iter_next(a_iter);
            g_sequence_remove(a_iter);
            a_iter = t;
        }else
            a_iter = g_sequence_iter_next(a_iter);
    }
}


//read data according to the pattern
static void read_data_by_pattern(int *pattern, GSequence *a, GList *b, int a_len, int b_len, unsigned char*write_buf){
    int i, j=0;
    GList* b_iter=b;
    int read_off, read_len=0;
    int buff_off=0, flag=0;
    
    //read data according to patterns
    GSequenceIter *a_iter = g_sequence_get_begin_iter(a);
    struct chunk *a_first = (struct chunk*)g_sequence_get(a_iter);
    containerid cid = a_first->id;
    
    while (j<b_len) {
        struct metaEntry* me = (struct metaEntry*)b_iter->data;
        //need to read chunks
        if (pattern[j]) {
            struct pattern_chunk *ch = malloc(sizeof(struct pattern_chunk));
            ch->me = me;
            ch->data = write_buf+read_len;
            g_hash_table_insert(ht_pattern_chunks, &me->fp, ch);
            if (flag==0) {
                read_off = me->off;
                read_len = me->len;
                flag = 1;
            }else
            read_len += me->len;
        }else if (flag==1) {
            flag = 0;
            read_data_in_container(cid, read_off, read_len, write_buf+buff_off);
            DEBUG("read %d data at offset %d ", read_len, read_off);
            buff_off += read_len;
        }
        
        j++;
        b_iter = g_list_next(b_iter);
    }
    if (flag==1) {
        read_data_in_container(cid, read_off, read_len, write_buf+buff_off);
        DEBUG("read %d data at offset %d ", read_len, read_off);
    }
    
    //assign data for each chunk in recipe list
    a_iter = g_sequence_get_begin_iter(a);
    for (i=0; i<a_len; i++) {
        struct chunk *ch = (struct chunk*)g_sequence_get(a_iter);
        //data has already read index by pattern hash table
        struct pattern_chunk *pch = g_hash_table_lookup(ht_pattern_chunks, &ch->fp);
        if (pch) {
            //copy data of chunks into the segment
            assert(ch->size == pch->me->len);
            ch->data = malloc(ch->size);
            assert(pch->data);
            memcpy(ch->data, pch->data, ch->size);
            
            GSequenceIter *t = g_sequence_iter_next(a_iter);
            g_sequence_remove(a_iter);
            a_iter = t;
        }
        else
        a_iter = g_sequence_iter_next(a_iter);
    }
    g_hash_table_remove_all(ht_pattern_chunks);
}

static int* generate_pattern(GSequence *a, GList *b, int a_len, int b_len){
    int i, j, k, d;
    int val = 1;
    
    //assert(a_len == g_sequence_get_length(a));
    //generate the pattern
    int *pattern = (int*)malloc(sizeof(int)*b_len);
    memset(pattern, 0, sizeof(int)*b_len);
    
    GHashTable *ht = g_hash_table_new(g_int_hash, (GEqualFunc)g_fingerprint_equal);
    
    GSequenceIter *a_iter = g_sequence_get_begin_iter(a);
    for (i=0; i<a_len; i++) {
        struct chunk *ch = (struct chunk*)g_sequence_get(a_iter);
        g_hash_table_insert(ht, &ch->fp, &val);
        a_iter = g_sequence_iter_next(a_iter);
    }
    GList *b_iter = b;
    for (j=0; j<b_len; j++) {
        struct metaEntry *me = (struct metaEntry*)b_iter->data;
        if (g_hash_table_lookup(ht, &me->fp)){
            pattern[j] = 1;
        }
        b_iter = g_list_next(b_iter);
    }
    g_hash_table_destroy(ht);
    
    
    //wild match the pattern
    for (j=0; j<b_len; j+=d) {
        while (pattern[j]) j++;
        if (j>=b_len)
        break;
        d=0;
        while (pattern[j+d] == 0) d++;
        if (j+d >=b_len)
        break;
        if (d<=destor.wildcard_length) {
            for (i=0; i<d; i++)
            pattern[j+i] = 2;
        }
    }
    //    //output the pattern
    //    printf("the pattern as follows: \n");
    //    for (i=0; i<b_len; i++) {
    //        printf("%d", pattern[i]);
    //    }
    //    printf("\n");
    
    return pattern;
}





//read data according to the pattern
static void read_data_by_merged_pattern(int *pattern, GSequence *a1, GSequence *a2, GList *b, int a1_len, int a2_len, int b_len, unsigned char*write_buf){
    int i, j=0;
    GList* b_iter=b;
    int read_off, read_len=0;
    int buff_off=0, flag=0;
    
    //read data according to patterns
    GSequenceIter *a_iter = g_sequence_get_begin_iter(a1);
    GSequenceIter *a1_end = g_sequence_get_end_iter(a1);
    struct chunk *a_first = (struct chunk*)g_sequence_get(a_iter);
    containerid cid = a_first->id;
    
    while (j<b_len) {
        struct metaEntry* me = (struct metaEntry*)b_iter->data;
        //need to read chunks
        if (pattern[j]) {
            struct pattern_chunk *ch = malloc(sizeof(struct pattern_chunk));
            ch->me = me;
            ch->data = write_buf+read_len;
            g_hash_table_insert(ht_pattern_chunks, &me->fp, ch);
            if (flag==0) {
                read_off = me->off;
                read_len = me->len;
                flag = 1;
            }else
            read_len += me->len;
        }else if (flag==1) {
            flag = 0;
            read_data_in_container(cid, read_off, read_len, write_buf+buff_off);
            DEBUG("read %d data at offset %d ", read_len, read_off);
            buff_off += read_len;
        }
        
        j++;
        b_iter = g_list_next(b_iter);
    }
    if (flag==1) {
        read_data_in_container(cid, read_off, read_len, write_buf+buff_off);
        DEBUG("read %d data at offset %d ", read_len, read_off);
    }
    
    //assign data for each chunk in recipe list
    a_iter = g_sequence_get_begin_iter(a1);
    
    for (i=0; i<a1_len+a2_len; i++) {
        if (a_iter == a1_end)
        a_iter = g_sequence_get_begin_iter(a2);
        
        struct chunk *ch = (struct chunk*)g_sequence_get(a_iter);
        //data has already read index by pattern hash table
        struct pattern_chunk *pch = g_hash_table_lookup(ht_pattern_chunks, &ch->fp);
        if (pch) {
            //copy data of chunks into the segment
            assert(ch->size == pch->me->len);
            ch->data = malloc(ch->size);
            assert(pch->data);
            memcpy(ch->data, pch->data, ch->size);
            
            GSequenceIter *t = g_sequence_iter_next(a_iter);
            g_sequence_remove(a_iter);
            a_iter = t;
        }
        else
        a_iter = g_sequence_iter_next(a_iter);
    }
    g_hash_table_remove_all(ht_pattern_chunks);
}



static int* generate_merged_pattern(GSequence *a1, GSequence *a2, GList *b, int a1_len, int a2_len, int b_len){
    int i, j, k, d;
    int val;
    //assert(a1_len == g_sequence_get_length(a1) && a2_len == g_sequence_get_length(a2));
    
    //generate the pattern
    int *pattern = (int*)malloc(sizeof(int)*b_len);
    memset(pattern, 0, sizeof(int)*b_len);
    
    GHashTable *ht = g_hash_table_new(g_int_hash, (GEqualFunc)g_fingerprint_equal);
    
    val = 2;
    GSequenceIter *a_iter = g_sequence_get_begin_iter(a2);
    for (i=0; i<a2_len; i++) {
        struct chunk *ch = (struct chunk*)g_sequence_get(a_iter);
        g_hash_table_insert(ht, &ch->fp, &val);
        a_iter = g_sequence_iter_next(a_iter);
    }
    
    val = 1;
    a_iter = g_sequence_get_begin_iter(a1);
    for (i=0; i<a1_len; i++) {
        struct chunk *ch = (struct chunk*)g_sequence_get(a_iter);
        g_hash_table_insert(ht, &ch->fp, &val);
        a_iter = g_sequence_iter_next(a_iter);
    }
    
    GList *b_iter = b;
    for (j=0; j<b_len; j++) {
        struct metaEntry *me = (struct metaEntry*)b_iter->data;
        int *pv = g_hash_table_lookup(ht, &me->fp);
        if (pv)
        pattern[j] = *pv;
        b_iter = g_list_next(b_iter);
    }
    g_hash_table_destroy(ht);
    
    
    
    //wild match the pattern
    for (j=0; j<b_len; j+=d) {
        while (pattern[j]) j++;
        if (j>=b_len)
        break;
        d=0;
        while (pattern[j+d] == 0) d++;
        if (j+d >=b_len)
        break;
        if (d<=destor.wildcard_length) {
            for (i=0; i<d; i++)
            pattern[j+i] = 3;
        }
    }
    //
    //        //output the pattern
    //        printf("the merged pattern as follows: \n");
    //        for (i=0; i<b_len; i++) {
    //            printf("%d", pattern[i]);
    //        }
    //        printf("\n");
    
    return pattern;
}

static void send_segment_to_restore (struct segment *s){
    if (destor.simulation_level == SIMULATION_NO) {
        GSequenceIter *end = g_sequence_get_end_iter(s->chunks);
        GSequenceIter *begin = g_sequence_get_begin_iter(s->chunks);
        while(begin != end) {
            struct chunk* c = g_sequence_get(begin);
            //fdl 
            if(!CHECK_CHUNK(c,CHUNK_FILE_START)&&!CHECK_CHUNK(c,CHUNK_FILE_END))
                jcr.data_size+=c->size;
            //fdl 
            sync_queue_push(restore_chunk_queue, c);
            
            g_sequence_remove(begin);
            begin = g_sequence_get_begin_iter(s->chunks);
        }
        s->chunk_num = 0;
    }
}

static GSequence* generate_unread_list(struct segment *s, int32_t *s_len){
    int t = 0;
    GSequence *s_chunk_list = g_sequence_new(NULL);
    if (s == NULL) {
        *s_len = t;
        return s_chunk_list;
    }
    
    GSequenceIter *s_iter = g_sequence_get_begin_iter(s->chunks);
    GSequenceIter *s_end = g_sequence_get_end_iter(s->chunks);
    for(; s_iter != s_end; s_iter = g_sequence_iter_next(s_iter)){
        struct chunk* ch = (struct chunk*)g_sequence_get(s_iter);
        if (CHECK_CHUNK(ch, CHUNK_FILE_START) || CHECK_CHUNK(ch, CHUNK_FILE_END ))
        continue;
        
        t++;
        g_sequence_append(s_chunk_list, ch);
    }
    *s_len = t;
    return s_chunk_list;
}

/*
static void remove_cached_chunks_in_unread_list(GSequence* s_list, int32_t* s_len){
    int len = g_sequence_get_length(s_list);
    GSequence *s_chunk_list = g_sequence_new(NULL);
    GSequenceIter *s_iter = g_sequence_get_begin_iter(s_list);
    GSequenceIter *s_end = g_sequence_get_end_iter(s_list);
    
    while(s_iter != s_end){
        struct chunk* ch = (struct chunk*)g_sequence_get(s_iter);
        struct container *con = lru_cache_lookup(dataCache, &ch->fp);
        if (con) {
            struct chunk *rc = get_chunk_in_container(con, &ch->fp);
            //NOTICE("hello world!");
            assert(rc);
        }
        s_iter = g_sequence_iter_next(s_iter);
    }
    *s_len = len;
}
*/
static void remove_cached_chunks_in_unread_list(GSequence* s_list, int32_t* s_len){
    int len = g_sequence_get_length(s_list);
    GSequence *s_chunk_list = g_sequence_new(NULL);
    GSequenceIter *s_iter = g_sequence_get_begin_iter(s_list);
    GSequenceIter *s_end = g_sequence_get_end_iter(s_list);
    
    while(s_iter != s_end){
        struct chunk* ch = (struct chunk*)g_sequence_get(s_iter);
        struct container *con = lru_cache_lookup(dataCache, &ch->fp);
        if (con) {
            struct chunk *rc = get_chunk_in_container(con, &ch->fp);
            //NOTICE("container cache hit!");
            assert(rc);
            assert(rc->data);
            assert(ch->size == rc->size);
            
            //ch->data = malloc(ch->size);
            //memcpy(ch->data, rc->data, ch->size);
            ch->data = rc->data;
            rc->data = NULL;
            free_chunk(rc);
            
            GSequenceIter *t = g_sequence_iter_next(s_iter);
            g_sequence_remove(s_iter);
            s_iter = t;
            len--;
        }
        else{
            s_iter = g_sequence_iter_next(s_iter);
        }
    }
    *s_len = len;
}




static int is_prefetch_container(int *pattern, int len, int chunk_num){
    int i, cnt=0;
    for (i=0; i<len; i++) {
        if (pattern[i]) {
            cnt++;
        }
    }
    
    if (cnt > destor.prefetch_container_percent*chunk_num){
        //NOTICE("cnt=%d;chunk_num=%d",cnt,chunk_num);
        return 1;
        
    }
    return 0;
}



void* pattern_restore_plus_thread(void *arg) {
    struct segment *s1 = NULL, *s2 = NULL;
    GSequence *s1_chunk_list, *s2_chunk_list;
    int32_t s1_cur_len, s2_cur_len;
    int32_t t_num;
    
    //# of meta data of containers
    metaCache = new_lru_cache(destor.size_of_meta_cache, (void*)free_container_meta, container_meta_check_id);
    //data cache
    
    if (destor.restore_cache[1]){
        dataCache = new_lru_cache(destor.restore_cache[1], (void*)free_container,
                              lookup_fingerprint_in_container);
    }
    
    struct chunk *c = NULL;
    while (!s1) {
        c = sync_queue_pop(restore_recipe_queue);
        /* Add the chunk to the segment1. */
        s1 = (struct segment*)segmenting(c);
    }
    s1_chunk_list = generate_unread_list(s1, &s1_cur_len);
    
    
    //index the pattern by hash table
    ht_pattern_chunks = g_hash_table_new_full(g_int_hash, (GEqualFunc)g_fingerprint_equal, NULL, free);
    
    while (1) {
        TIMER_DECLARE(1);
        TIMER_BEGIN(1);
        assert(s2 == NULL);
        //generate chunk list of s2 by looking forward
        while (c && !s2) {
            c = sync_queue_pop(restore_recipe_queue);
            /* Add the chunk to the segment2. */
            s2 = (struct segment*)segmenting(c);
        }
        
        GSequence *s2_chunk_list = g_sequence_new(NULL);
        s2_cur_len = 0;
        if (s2) {
            s2_chunk_list = generate_unread_list(s2, &s2_cur_len);
        }
        
        //lookup in data cache
        if (destor.restore_cache[1]) {
            //check and remove cached chunks in s1 list
            if(s1_cur_len)
            remove_cached_chunks_in_unread_list(s1_chunk_list, &s1_cur_len);
            if(s2_cur_len)
            remove_cached_chunks_in_unread_list(s2_chunk_list, &s2_cur_len);
        }
        
        DEBUG("\n\nat first s1 chunk list is %d (to read chunks:%d) and s2 chunk list is %d(to read chunks:%d)",
              s1->chunk_num, s1_cur_len, s2?s2->chunk_num:0, s2_cur_len);
        
        
        //compute the patterns in s1
        
        while (s1_cur_len) {
            GSequenceIter *s1_iter = g_sequence_get_begin_iter(s1_chunk_list);
            struct chunk* ch = (struct chunk*)g_sequence_get(s1_iter);
            //fdl 
            //^^^^^^^^^^^TIMER_DECLARE(1);
            //^^^^^^^^^^^^TIMER_BEGIN(1);
            //fdl 
            
            //read the container meta data by the container id of the chunk
            //cache a certain number of container meta data
            struct containerMeta *me = lru_cache_lookup(metaCache, &ch->id);
//I need to Add Add Add!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!


            if (!me) {
                DEBUG("meta cache: container %lld is missed and load it from storage", ch->id);
                //not found in meta cache, then get the meta data from the lower storage and store into meta cache
                me = retrieve_container_meta_by_id(ch->id);
                assert(lookup_fingerprint_in_container_meta(me, &ch->fp));
                lru_cache_insert(metaCache, me, NULL, NULL);
                jcr.read_container_num++;
            }
            assert(me);
            
            //locate the start position of chunk list of the container
            GList *t_chunk_list = g_hash_table_lookup(me->map, &ch->fp);
            assert(t_chunk_list);
            t_num = g_list_length(t_chunk_list);
            DEBUG("the found chunk list length is %d in the container (%d)", t_num, ch->id);
            assert(t_num);
            

            int *pattern;
            unsigned char* buff=NULL;
            if (s1_cur_len >= s1->chunk_num / 2|| s2_cur_len == 0) {
                //NOTICE("go s1 way---0");
                //generate patterns in seqence s1 and t
                pattern = generate_pattern(s1_chunk_list, t_chunk_list, s1_cur_len, t_num);
                
                
                if (destor.restore_cache[1]&&is_prefetch_container(pattern, t_num, me->chunk_num)) {
                    struct container* con = retrieve_container_by_id(ch->id);
                    //NOTICE("hello thread1!\n");
                    lru_cache_insert(dataCache, con, NULL, NULL);
                    //jcr.read_container_num++;
                    //NOTICE("s1 insert into dataCache,dataCache.size=%d",dataCache->size);
                    read_data_by_pattern_in_data_cache(s1_chunk_list,s1_cur_len, con);
                }else{
                    //NOTICE("go s1 way---1");
                    //allocate data space
                    buff = allocate_data_buffer(pattern, t_chunk_list, t_num);
                    //NOTICE("go s1 way---2");
                    //read data
                    read_data_by_pattern(pattern, s1_chunk_list, t_chunk_list, s1_cur_len, t_num, buff);
                    //NOTICE("go s1 way---3");
                }
                //^^^^^^^^^^^^^^^^TIMER_END(1,jcr.read_chunk_time);
            }else {
                //NOTICE("go s1 and s2 way---0");
                //generate the merged patterns in seqence s1, s2 and t
                pattern = generate_merged_pattern(s1_chunk_list, s2_chunk_list, t_chunk_list, s1_cur_len, s2_cur_len, t_num);
                //NOTICE("go s1 and s2 way---1");
                if (destor.restore_cache[1]&&is_prefetch_container(pattern, t_num, me->chunk_num)) {
                    struct container* con = retrieve_container_by_id(ch->id);
                    //NOTICE("hello thread2!\n");
                    lru_cache_insert(dataCache, con, NULL, NULL);
                    //NOTICE("s2 insert into dataCache,dataCache.size=%d",dataCache->size);
                    //jcr.read_container_num++;
                    read_data_by_pattern_in_data_cache(s1_chunk_list,s1_cur_len, con);
                }else{
                    //allocate data space
                    buff = allocate_data_buffer(pattern, t_chunk_list, t_num);
                    //NOTICE("go s1 and s2 way---2");
                    //read data
                    read_data_by_merged_pattern(pattern, s1_chunk_list, s2_chunk_list, t_chunk_list, s1_cur_len, s2_cur_len, t_num, buff);
                    //NOTICE("go s1 and s2 way---3");
                }
                //^^^^^^^^^^^^^^TIMER_END(1,jcr.read_chunk_time);
            }
            
            assert(s1_chunk_list);
            s1_cur_len = g_sequence_get_length(s1_chunk_list);
            assert(s2_chunk_list);
            s2_cur_len = g_sequence_get_length(s2_chunk_list);
            DEBUG("the remained length of s1 is %d and s2 is %d", s1_cur_len, s2_cur_len);
            
            if (buff) {
                free(buff);
            }
            free(pattern);
        }
        TIMER_END(1,jcr.read_chunk_time);

        assert(s1_cur_len==0);
        g_sequence_free(s1_chunk_list);
        s1_chunk_list = s2_chunk_list;
        s1_cur_len = s2_cur_len;
        s2_chunk_list = NULL;
        s2_cur_len = 0;
        
        DEBUG("send a segment which has %d chunks to restore", s1->chunk_num);

        //fdl
        jcr.chunk_num+=s1->chunk_num;
        //fdl
        //send chunks into next phase
        //^^^^^^^^^^^^^TIMER_DECLARE(1);
        //^^^^^^^^^^^TIMER_BEGIN(1);
        send_segment_to_restore(s1);
        //^^^^^^^^^^^^TIMER_END(1,jcr.read_chunk_time);
        assert(s1->chunk_num == 0);
        free_segment(s1);
        
        s1 = s2;
        s2 = NULL;
        if (c == NULL && s1==NULL)
        break;
    }
    sync_queue_term(restore_chunk_queue);
    if (s1_chunk_list) {
        g_sequence_free(s1_chunk_list);
    }
    
    
    g_hash_table_destroy(ht_pattern_chunks);
    free_lru_cache(metaCache);
    if (destor.restore_cache[1]) {
        free_lru_cache(dataCache);
    }
    
    return NULL;
}


