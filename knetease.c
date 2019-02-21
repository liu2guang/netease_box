#include "rtdevice.h"
#include "rtthread.h"
#include "webclient.h"
#include "dfs_posix.h"
#include "cJson.h" 

#define DBG_SECTION_NAME "knetease"
#define DBG_ENABLE
#define DBG_LEVEL DBG_LOG
#define DBG_COLOR
#include <rtdbg.h>

#define Knetease_SearchMusic  "http://music.163.com/api/search/pc?s=%s&offset=%d&limit=1&type=%d"  
#define Knetease_PlayMusic    "http://music.163.com/song/media/outer/url?id=%d.mp3"  
#define Knetease_GetLyric     "http://music.163.com/api/song/lyric?id=%d&lv=-1&kv=-1&tv=1"  
#define Knetease_GetInfo      "http://music.163.com/api/song/detail?id=%d&ids=[%d]" 
#define Knetease_RespBuffSize (20*1024)
#define Knetease_ThreadStack  (2 *1024) 

struct knetease_music
{
    char *name;
    rt_uint32_t id;
}; 
typedef struct knetease_music* knetease_music_t; 

enum knetease_search_type
{
    K_SEARCH_TYPE_MUSIC    =    1, /* 歌曲 */ 
    K_SEARCH_TYPE_ALBUM    =   10, /* 专辑 */ 
    K_SEARCH_TYPE_SINGER   =  100, /* 歌手 */
    K_SEARCH_TYPE_PLAYLIST = 1000, /* 歌单 */
    K_SEARCH_TYPE_USER     = 1002, /* 用户 */
    K_SEARCH_TYPE_MV       = 1004, /* 短片 */
    K_SEARCH_TYPE_LYRICS   = 1006, /* 歌词 */
    K_SEARCH_TYPE_RADIO    = 1009  /* 电台 */
}; 
typedef enum knetease_search_type knetease_search_type_t; 

/* 搜索功能 */
#define KNE_URL_SIZE     (256)
#define KNE_RespBuffSize (10*1024)
static knetease_music_t knetease_search(const char *name, rt_uint32_t offset, knetease_search_type_t type) 
{
    knetease_music_t music = RT_NULL; 
    char url[KNE_URL_SIZE] = {0}; 
    struct webclient_session *session = RT_NULL;
    char *resp_buff = RT_NULL; 

    /* 合成搜索URL */
    rt_snprintf(url, KNE_URL_SIZE, Knetease_SearchMusic, name, offset, type);  
    // LOG_D("Get URL: %s.", url); 

    /* 创建HTTP连接 */ 
    session = webclient_open(url);
    if(session == RT_NULL)
    {
        LOG_E("open website:%s failed.", url); 
        goto _ret;
    }
    else
    {
        if(session->response != 200)
        {
            LOG_E("wrong response: %d.", session->response);
            goto _ret;
        }

        if(session->content_length > KNE_RespBuffSize)
        {
            LOG_E("content is greater than 10 bytes. current is %d bytes.", KNE_RespBuffSize, session->content_length);
            goto _ret;
        }
    }

    /* 获取响应 */ 
    {
        int length  = (-1); 
        int offset  = (-1);
        int residue = (-1);

        resp_buff = (char *)rt_malloc(KNE_RespBuffSize);
        if(resp_buff == RT_NULL)
        {
            LOG_E("resp_buff malloc failed, out of memory.");
            goto _ret;
        } 

        for (offset = 0; (length > 0) || (length == (-1)); offset += length)
        {
            residue = session->content_length - offset; 
            length = webclient_read(session, (unsigned char *)resp_buff, residue);
        }
    }

    /* 解析响应 */
    {
        cJSON *root   = RT_NULL;
        cJSON *result = RT_NULL; 
        cJSON *songs  = RT_NULL; 
        cJSON *name   = RT_NULL; 
        cJSON *id     = RT_NULL; 

        root = cJSON_Parse((const char *)resp_buff);  
        if(root == RT_NULL)
        {
            LOG_E("parse json string failed: %s.", cJSON_GetErrorPtr());
            goto _ret; 
        }

        /* 格式化输出 */ 
        // #if defined(DBG_ENABLE)
        // {
        //     char *string = cJSON_Print(root); 
        //     LOG_D("Json: %s", string); 
        //     rt_free(string);  
        // }
        // #endif

        result = cJSON_GetObjectItem(root, "result"); 
        if(result == RT_NULL)
        {
            LOG_E("parse result json obj failed: %s.", cJSON_GetErrorPtr());
            goto _ret;  
        }
        else
        {
            songs = cJSON_GetObjectItem(result, "songs");
            if(songs == RT_NULL)
            {
                LOG_E("parse songs json obj failed: %s.", cJSON_GetErrorPtr());
                goto _ret;  
            }
            else
            {
                name = cJSON_GetObjectItem(songs->child, "name");
                id   = cJSON_GetObjectItem(songs->child, "id");

                if((name == RT_NULL) || (id == RT_NULL))
                {
                    LOG_E("parse name or id json obj failed: %s.", cJSON_GetErrorPtr());
                    goto _ret;  
                } 
            }
        }

        /* 合成结构体 */
        music = (knetease_music_t)rt_malloc(sizeof(struct knetease_music)); 
        if(music == RT_NULL)
        {
            LOG_E("kanime_music struct malloc failed, out of memory.");
            goto _ret; 
        } 

        music->name = rt_strdup(name->valuestring);
        music->id   = id->valueint;

        // LOG_D("Get music name: %s, id: %d.", music->name, music->id); 

        /* 释放中间使用变量 */
        if(root != RT_NULL)
        {
            cJSON_Delete(root);
            root   = RT_NULL;
            result = RT_NULL;
            songs  = RT_NULL;
            name   = RT_NULL;
            id     = RT_NULL;
        }
    }

_ret: 
    if(resp_buff != RT_NULL)
    {
        rt_free(resp_buff);
        resp_buff = RT_NULL; 
    }

    if(session != RT_NULL)
    {
        webclient_close(session);
        session = RT_NULL; 
    }

    return music;
}
#undef KNE_URL_SIZE
#undef KNE_RespBuffSize

/* 删除歌 */ 
static rt_err_t knetease_free(knetease_music_t music)
{
    if(music->name != RT_NULL)
    {
        rt_free(music->name); 
        music->name = RT_NULL; 
    }
    
    if(music != RT_NULL)
    {
        rt_free(music); 
        music = RT_NULL; 
    }    

    return RT_EOK;
}

#define KNE_URL_SIZE     (256)
#define KNE_RespBuffSize (10*1024)
static rt_err_t knetease_get_lyric(knetease_music_t music) 
{
    char url[KNE_URL_SIZE] = {0}; 
    struct webclient_session *session = RT_NULL;
    char *resp_buff = RT_NULL; 

    /* 合成搜索URL */
    rt_snprintf(url, KNE_URL_SIZE, Knetease_GetLyric, music->id);  
    LOG_D("Get URL: %s.", url); 

    /* 创建HTTP连接 */ 
    session = webclient_open(url);
    if(session == RT_NULL)
    {
        LOG_E("open website:%s failed.", url); 
        goto _ret;
    }
    else
    {
        if(session->response != 200)
        {
            LOG_E("wrong response: %d.", session->response);
            goto _ret;
        }

        if(session->content_length > KNE_RespBuffSize)
        {
            LOG_E("content is greater than 10 bytes. current is %d bytes.", KNE_RespBuffSize, session->content_length);
            goto _ret;
        }
    }

    /* 获取响应 */ 
    {
        int length  = (-1); 
        int offset  = (-1);
        int residue = (-1);

        resp_buff = (char *)rt_malloc(KNE_RespBuffSize);
        if(resp_buff == RT_NULL)
        {
            LOG_E("resp_buff malloc failed, out of memory.");
            goto _ret;
        } 

        for (offset = 0; (length > 0) || (length == (-1)); offset += length)
        {
            residue = session->content_length - offset; 
            length = webclient_read(session, (unsigned char *)resp_buff, residue);
        }
    }

    /* 解析响应 */
    {
        cJSON *root   = RT_NULL;

        root = cJSON_Parse((const char *)resp_buff);  
        if(root == RT_NULL)
        {
            LOG_E("parse json string failed: %s.", cJSON_GetErrorPtr());
            goto _ret; 
        }

        /* 格式化输出 */ 
        #if defined(DBG_ENABLE)
        {
            char *string = cJSON_Print(root); 
            LOG_D("Json: %s", string); 
            rt_free(string);  
        }
        #endif

        /* 释放中间使用变量 */
        if(root != RT_NULL)
        {
            cJSON_Delete(root);
            root = RT_NULL;
        }
    }

_ret: 
    if(resp_buff != RT_NULL)
    {
        rt_free(resp_buff);
        resp_buff = RT_NULL; 
    }

    if(session != RT_NULL)
    {
        webclient_close(session);
        session = RT_NULL; 
    }

    return RT_EOK;
}
#undef KNE_URL_SIZE
#undef KNE_RespBuffSize

#define KNE_URL_SIZE     (256)
#define KNE_RespBuffSize (10*1024)
static rt_err_t knetease_get_info(knetease_music_t music) 
{
    char url[KNE_URL_SIZE] = {0}; 
    struct webclient_session *session = RT_NULL;
    char *resp_buff = RT_NULL; 

    /* 合成搜索URL */
    rt_snprintf(url, KNE_URL_SIZE, Knetease_GetInfo, music->id, music->id);  
    LOG_D("Get URL: %s.", url); 

    /* 创建HTTP连接 */ 
    session = webclient_open(url);
    if(session == RT_NULL)
    {
        LOG_E("open website:%s failed.", url); 
        goto _ret;
    }
    else
    {
        if(session->response != 200)
        {
            LOG_E("wrong response: %d.", session->response);
            goto _ret;
        }

        if(session->content_length > KNE_RespBuffSize)
        {
            LOG_E("content is greater than 10 bytes. current is %d bytes.", KNE_RespBuffSize, session->content_length);
            goto _ret;
        }
    }

    /* 获取响应 */ 
    {
        int length  = (-1); 
        int offset  = (-1);
        int residue = (-1);

        resp_buff = (char *)rt_malloc(KNE_RespBuffSize);
        if(resp_buff == RT_NULL)
        {
            LOG_E("resp_buff malloc failed, out of memory.");
            goto _ret;
        } 

        for (offset = 0; (length > 0) || (length == (-1)); offset += length)
        {
            residue = session->content_length - offset; 
            length = webclient_read(session, (unsigned char *)resp_buff, residue);
        }
    }

    /* 解析响应 */
    {
        cJSON *root = RT_NULL;

        root = cJSON_Parse((const char *)resp_buff);  
        if(root == RT_NULL)
        {
            LOG_E("parse json string failed: %s.", cJSON_GetErrorPtr());
            goto _ret; 
        }

        /* 格式化输出 */ 
        #if defined(DBG_ENABLE)
        {
            char *string = cJSON_Print(root); 
            LOG_D("Json: %s", string); 
            rt_free(string);  
        }
        #endif

        /* 释放中间使用变量 */
        if(root != RT_NULL)
        {
            cJSON_Delete(root);
            root = RT_NULL;
        }
    }

_ret: 
    if(resp_buff != RT_NULL)
    {
        rt_free(resp_buff);
        resp_buff = RT_NULL; 
    }

    if(session != RT_NULL)
    {
        webclient_close(session);
        session = RT_NULL; 
    }

    return RT_EOK;
}
#undef KNE_URL_SIZE
#undef KNE_RespBuffSize

/* 播放指定ID歌曲 */
#define KNE_URL_SIZE (256)
static rt_err_t knetease_play_by_id(rt_uint32_t id) 
{
    rt_err_t ret = RT_EOK;
    char url[KNE_URL_SIZE] = {0}; 
    
    /* 合成搜索URL */
    rt_snprintf(url, KNE_URL_SIZE, Knetease_PlayMusic, id);  
    // LOG_D("Play URL: %s.", url); 
    
    // Todo
    // 使用url播放音乐, 异步播放

    return ret; 
}
#undef KNE_URL_SIZE

static int _knetease_search(int argc, char *argv[]) 
{
    knetease_music_t music = RT_NULL;

    if(argc < 2)
    {
        LOG_E("use: knetease_search musicname index."); 
        return (-RT_ERROR); 
    }
    else
    {
        music = knetease_search(argv[1], atoi(argv[2]), K_SEARCH_TYPE_MUSIC); 
        //knetease_get_lyric(music); 
        knetease_get_info(music); 
        knetease_play_by_id(music->id);
        knetease_free(music);  
    }

    return RT_EOK; 
}
MSH_CMD_EXPORT_ALIAS(_knetease_search, knetease_search, search music from netease.); 

static void knetease_run(void *p) 
{
    LOG_I("knetease thread start run."); 

    while(1)
    {
        rt_thread_mdelay(1000);
    }
}

int knetease_init(void)
{
    rt_err_t ret = RT_EOK;
    rt_thread_t thread = RT_NULL;

    thread = rt_thread_create("knetease", knetease_run, RT_NULL, Knetease_ThreadStack, 16, 10);
    if (thread == RT_NULL)
    {
        LOG_E("knetease thread create failed.");
        ret = (-RT_ERROR);
        goto _ret;
    }

    rt_thread_startup(thread);

_ret:
    return ret;
}
