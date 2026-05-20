#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_system.h"
#include "m_misc.h"
#include "umapinfo.h"
#include "w_wad.h"

typedef enum
{
    tok_eof,
    tok_word,
    tok_string,
    tok_lbrace,
    tok_rbrace,
    tok_equals,
    tok_comma
} token_type_t;

typedef struct
{
    token_type_t type;
    char text[256];
} token_t;

typedef struct
{
    const char *p;
    token_t pushed;
    boolean has_pushed;
} parser_t;

static umapinfo_map_t maps[UMAPINFO_MAX_MAPS];
static int num_maps;

static umapinfo_episode_t episodes[UMAPINFO_MAX_EPISODES];
static boolean episode_present[UMAPINFO_MAX_EPISODES];

static const char *NonEmpty(const char *s)
{
    return s[0] != '\0' ? s : NULL;
}

static void CopyString(char *dest, size_t dest_size, const char *src)
{
    M_StringCopy(dest, src, dest_size);
}

static void CopyLumpName(char dest[9], const char *src)
{
    size_t i;

    for (i = 0; i < 8 && src[i] != '\0'; ++i)
    {
        dest[i] = toupper((unsigned char) src[i]);
    }

    dest[i] = '\0';
}

static void SkipWhitespaceAndComments(parser_t *parser)
{
    for (;;)
    {
        while (isspace((unsigned char) *parser->p))
        {
            ++parser->p;
        }

        if (parser->p[0] == '/' && parser->p[1] == '/')
        {
            parser->p += 2;
            while (*parser->p != '\0' && *parser->p != '\n')
            {
                ++parser->p;
            }
            continue;
        }

        if (parser->p[0] == '/' && parser->p[1] == '*')
        {
            parser->p += 2;
            while (*parser->p != '\0'
                && !(parser->p[0] == '*' && parser->p[1] == '/'))
            {
                ++parser->p;
            }
            if (*parser->p != '\0')
            {
                parser->p += 2;
            }
            continue;
        }

        break;
    }
}

static token_t NextToken(parser_t *parser)
{
    token_t token;
    size_t len;

    if (parser->has_pushed)
    {
        parser->has_pushed = false;
        return parser->pushed;
    }

    token.type = tok_eof;
    token.text[0] = '\0';

    SkipWhitespaceAndComments(parser);

    switch (*parser->p)
    {
      case '\0':
        token.type = tok_eof;
        return token;

      case '{':
        ++parser->p;
        token.type = tok_lbrace;
        return token;

      case '}':
        ++parser->p;
        token.type = tok_rbrace;
        return token;

      case '=':
        ++parser->p;
        token.type = tok_equals;
        return token;

      case ',':
        ++parser->p;
        token.type = tok_comma;
        return token;

      case '"':
        ++parser->p;
        token.type = tok_string;
        len = 0;

        while (*parser->p != '\0' && *parser->p != '"')
        {
            char c = *parser->p++;

            if (c == '\\' && *parser->p != '\0')
            {
                c = *parser->p++;
            }

            if (len + 1 < sizeof(token.text))
            {
                token.text[len++] = c;
            }
        }

        if (*parser->p == '"')
        {
            ++parser->p;
        }

        token.text[len] = '\0';
        return token;

      default:
        token.type = tok_word;
        len = 0;

        while (*parser->p != '\0'
            && !isspace((unsigned char) *parser->p)
            && *parser->p != '{'
            && *parser->p != '}'
            && *parser->p != '='
            && *parser->p != ',')
        {
            if (len + 1 < sizeof(token.text))
            {
                token.text[len++] = *parser->p;
            }
            ++parser->p;
        }

        token.text[len] = '\0';
        return token;
    }
}

static void PushToken(parser_t *parser, token_t token)
{
    parser->pushed = token;
    parser->has_pushed = true;
}

static boolean IsValueToken(token_t token)
{
    return token.type == tok_word || token.type == tok_string;
}

boolean UMAPINFO_ResolveMap(const char *marker, int *episode, int *map)
{
    char name[9];

    CopyLumpName(name, marker);

    if (name[0] == 'E'
     && isdigit((unsigned char) name[1])
     && name[2] == 'M'
     && isdigit((unsigned char) name[3])
     && name[4] == '\0')
    {
        *episode = name[1] - '0';
        *map = name[3] - '0';
        return true;
    }

    if (name[0] == 'M'
     && name[1] == 'A'
     && name[2] == 'P'
     && isdigit((unsigned char) name[3])
     && isdigit((unsigned char) name[4])
     && name[5] == '\0')
    {
        *episode = 1;
        *map = (name[3] - '0') * 10 + name[4] - '0';
        return true;
    }

    return false;
}

static umapinfo_map_t *GetOrCreateMap(const char *marker)
{
    char lump[9];
    int episode;
    int map;
    int i;

    CopyLumpName(lump, marker);

    if (!UMAPINFO_ResolveMap(lump, &episode, &map))
    {
        return NULL;
    }

    for (i = 0; i < num_maps; ++i)
    {
        if (!strcmp(maps[i].lump, lump))
        {
            return &maps[i];
        }
    }

    if (num_maps >= UMAPINFO_MAX_MAPS)
    {
        return NULL;
    }

    memset(&maps[num_maps], 0, sizeof(maps[num_maps]));
    CopyString(maps[num_maps].lump, sizeof(maps[num_maps].lump), lump);
    maps[num_maps].episode = episode;
    maps[num_maps].map = map;
    return &maps[num_maps++];
}

static void MarkEpisodeMap(int episode)
{
    if (episode < 1 || episode > UMAPINFO_MAX_EPISODES)
    {
        return;
    }

    if (!episode_present[episode - 1])
    {
        memset(&episodes[episode - 1], 0, sizeof(episodes[episode - 1]));
        episodes[episode - 1].episode = episode;
        M_snprintf(episodes[episode - 1].patch,
                   sizeof(episodes[episode - 1].patch),
                   "M_EPI%d", episode);
        M_snprintf(episodes[episode - 1].name,
                   sizeof(episodes[episode - 1].name),
                   "Episode %d", episode);
        episodes[episode - 1].key = '0' + episode;
        episode_present[episode - 1] = true;
    }
}

static void SetEpisodeInfo(int episode,
                           const char *patch,
                           const char *name,
                           const char *key)
{
    if (episode < 1 || episode > UMAPINFO_MAX_EPISODES)
    {
        return;
    }

    MarkEpisodeMap(episode);

    if (patch != NULL && patch[0] != '\0')
    {
        CopyLumpName(episodes[episode - 1].patch, patch);
    }

    if (name != NULL && name[0] != '\0')
    {
        CopyString(episodes[episode - 1].name,
                   sizeof(episodes[episode - 1].name), name);
    }

    if (key != NULL && key[0] != '\0')
    {
        episodes[episode - 1].key = tolower((unsigned char) key[0]);
    }
}

static void AppendInterText(umapinfo_map_t *map, const char *line)
{
    if (map->inter_text[0] != '\0')
    {
        M_StringConcat(map->inter_text, "\n", sizeof(map->inter_text));
    }

    M_StringConcat(map->inter_text, line, sizeof(map->inter_text));
}

static void ParseAssignment(parser_t *parser, umapinfo_map_t *map,
                            const char *key)
{
    token_t values[16];
    int num_values = 0;
    token_t token;

    token = NextToken(parser);
    if (token.type != tok_equals)
    {
        PushToken(parser, token);
        return;
    }

    for (;;)
    {
        token = NextToken(parser);
        if (!IsValueToken(token))
        {
            PushToken(parser, token);
            break;
        }

        if (num_values < arrlen(values))
        {
            values[num_values++] = token;
        }

        token = NextToken(parser);
        if (token.type != tok_comma)
        {
            PushToken(parser, token);
            break;
        }
    }

    if (num_values == 0)
    {
        return;
    }

    if (!strcasecmp(key, "LevelName"))
    {
        CopyString(map->level_name, sizeof(map->level_name), values[0].text);
    }
    else if (!strcasecmp(key, "LevelPic"))
    {
        CopyLumpName(map->level_pic, values[0].text);
    }
    else if (!strcasecmp(key, "Next"))
    {
        CopyLumpName(map->next, values[0].text);
    }
    else if (!strcasecmp(key, "NextSecret"))
    {
        CopyLumpName(map->next_secret, values[0].text);
    }
    else if (!strcasecmp(key, "Music"))
    {
        CopyLumpName(map->music, values[0].text);
    }
    else if (!strcasecmp(key, "SkyTexture"))
    {
        CopyLumpName(map->sky_texture, values[0].text);
    }
    else if (!strcasecmp(key, "ExitPic"))
    {
        CopyLumpName(map->exit_pic, values[0].text);
    }
    else if (!strcasecmp(key, "EndPic"))
    {
        CopyLumpName(map->end_pic, values[0].text);
    }
    else if (!strcasecmp(key, "InterText"))
    {
        int i;

        map->inter_text[0] = '\0';
        for (i = 0; i < num_values; ++i)
        {
            AppendInterText(map, values[i].text);
        }
    }
    else if (!strcasecmp(key, "Partime"))
    {
        map->partime = atoi(values[0].text);
    }
    else if (!strcasecmp(key, "Episode"))
    {
        SetEpisodeInfo(map->episode,
                       num_values > 0 ? values[0].text : NULL,
                       num_values > 1 ? values[1].text : NULL,
                       num_values > 2 ? values[2].text : NULL);
    }
}

static void ParseMapBlock(parser_t *parser, const char *marker)
{
    umapinfo_map_t *map;
    token_t token;

    map = GetOrCreateMap(marker);
    if (map == NULL)
    {
        return;
    }

    MarkEpisodeMap(map->episode);

    token = NextToken(parser);
    if (token.type != tok_lbrace)
    {
        PushToken(parser, token);
        return;
    }

    for (;;)
    {
        token = NextToken(parser);

        if (token.type == tok_eof || token.type == tok_rbrace)
        {
            break;
        }

        if (token.type == tok_word)
        {
            ParseAssignment(parser, map, token.text);
        }
    }
}

static void ParseText(const char *text)
{
    parser_t parser;
    token_t token;

    parser.p = text;
    parser.has_pushed = false;

    for (;;)
    {
        token = NextToken(&parser);

        if (token.type == tok_eof)
        {
            break;
        }

        if (token.type == tok_word && !strcasecmp(token.text, "map"))
        {
            token = NextToken(&parser);
            if (IsValueToken(token))
            {
                ParseMapBlock(&parser, token.text);
            }
        }
    }
}

void UMAPINFO_LoadLumps(void)
{
    int i;

    num_maps = 0;
    memset(maps, 0, sizeof(maps));
    memset(episodes, 0, sizeof(episodes));
    memset(episode_present, 0, sizeof(episode_present));

    for (i = 0; i < numlumps; ++i)
    {
        int len;
        char *text;

        if (strncasecmp(lumpinfo[i]->name, "UMAPINFO", 8))
        {
            continue;
        }

        len = W_LumpLength(i);
        text = malloc((size_t) len + 1);

        if (text == NULL)
        {
            I_Error("UMAPINFO_LoadLumps: out of memory");
        }

        W_ReadLump(i, text);
        text[len] = '\0';
        ParseText(text);
        free(text);
    }
}

const umapinfo_map_t *UMAPINFO_GetMap(int episode, int map)
{
    int i;

    for (i = 0; i < num_maps; ++i)
    {
        if (maps[i].episode == episode && maps[i].map == map)
        {
            return &maps[i];
        }
    }

    return NULL;
}

const umapinfo_episode_t *UMAPINFO_GetEpisode(int episode)
{
    if (episode < 1 || episode > UMAPINFO_MAX_EPISODES
     || !episode_present[episode - 1])
    {
        return NULL;
    }

    return &episodes[episode - 1];
}

boolean UMAPINFO_HasEpisode(int episode)
{
    return UMAPINFO_GetEpisode(episode) != NULL;
}

int UMAPINFO_MaxEpisode(void)
{
    int i;

    for (i = UMAPINFO_MAX_EPISODES; i >= 1; --i)
    {
        if (episode_present[i - 1])
        {
            return i;
        }
    }

    return 0;
}

boolean UMAPINFO_GetNextMap(int episode, int map, boolean secret,
                            int *next_episode, int *next_map)
{
    const umapinfo_map_t *info;
    const char *next;

    info = UMAPINFO_GetMap(episode, map);
    if (info == NULL)
    {
        return false;
    }

    next = secret && info->next_secret[0] != '\0'
         ? info->next_secret : info->next;

    if (next[0] == '\0')
    {
        return false;
    }

    return UMAPINFO_ResolveMap(next, next_episode, next_map);
}

const char *UMAPINFO_LevelName(int episode, int map)
{
    const umapinfo_map_t *info = UMAPINFO_GetMap(episode, map);
    return info != NULL ? NonEmpty(info->level_name) : NULL;
}

const char *UMAPINFO_LevelPic(int episode, int map)
{
    const umapinfo_map_t *info = UMAPINFO_GetMap(episode, map);
    return info != NULL ? NonEmpty(info->level_pic) : NULL;
}

const char *UMAPINFO_Music(int episode, int map)
{
    const umapinfo_map_t *info = UMAPINFO_GetMap(episode, map);
    return info != NULL ? NonEmpty(info->music) : NULL;
}

const char *UMAPINFO_SkyTexture(int episode, int map)
{
    const umapinfo_map_t *info = UMAPINFO_GetMap(episode, map);
    return info != NULL ? NonEmpty(info->sky_texture) : NULL;
}

const char *UMAPINFO_ExitPic(int episode, int map)
{
    const umapinfo_map_t *info = UMAPINFO_GetMap(episode, map);
    return info != NULL ? NonEmpty(info->exit_pic) : NULL;
}

const char *UMAPINFO_EndPic(int episode, int map)
{
    const umapinfo_map_t *info = UMAPINFO_GetMap(episode, map);
    return info != NULL ? NonEmpty(info->end_pic) : NULL;
}

const char *UMAPINFO_InterText(int episode, int map)
{
    const umapinfo_map_t *info = UMAPINFO_GetMap(episode, map);
    return info != NULL ? NonEmpty(info->inter_text) : NULL;
}

int UMAPINFO_ParTime(int episode, int map)
{
    const umapinfo_map_t *info = UMAPINFO_GetMap(episode, map);
    return info != NULL ? info->partime : 0;
}
