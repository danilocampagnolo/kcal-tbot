#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <jansson.h>
#include <ctype.h>
#include <stdbool.h>
#include <errno.h>

/* ======================================== */
/* == 1. Constants and Configurations == */
/* ======================================== */

#define VERSION "0.5"
#define MAX_URL_LENGTH 512
#define MAX_POST_DATA_LENGTH 4096 // Increased for potentially larger DeepSeek payloads
#define MAX_CHAT_ID_LENGTH 32
#define MAX_REPLY_LENGTH 18192 // Increased from 4096 to allow for larger replies
#define TELEGRAM_API_BASE "https://api.telegram.org/bot"
#define DEEPSEEK_API_ENDPOINT "http://localhost:11434/v1/chat/completions" // Default DeepSeek endpoint
#define DEFAULT_DEEPSEEK_MODEL "deepseek-r1:14b"                           // Default model name
static const char *SYSTEM_PROMPT =
    "Proceed like this:"
    "1. Estimate the different aliments in the description, and the full weight of the each food the user eaten, if no specific amount was written (otherwise use the specified amount). When doing so, first estimate and write down how many calories there is in a single item (for instance one apple), then multiply for the number specified by the user."
    "2. Split foods in their components. Like 100 grams of pasta with tomatoe is two items (if not three because of the oil), and so forth."
    "3. Then adjust the calories and macros depending on the table specification (for instance if the table is for 100 grams, and the weight of what the user eaten is 10, of course we need to adjust). Finally make sure that the macros and the calories are correctly proportional."
    "4. Consider that 1 gram of carbo have 4 kcal, 1 gram of proteins 3.5 kcal, 1 gram of fat 9kcal."
    "5. In the estimate of ingredients and quantities, consider the potential country the user lives based on the used query language."
    ""
    "Steps to follow to produce the reply:"
    "- First fo a step of reasoning where all the macros per 100 grams of products are shown, plus the reasoning steps needed to provide a good answer."
    "- Report the final answer as in the followind example (not that the ezample may not be accurate, it's just for formatting)."
    "- The json will have the TOTAL of calories and macros."
    "- Don't write anything past </reply>, but write your rasoning before <reply>."
    "- For the \"text\" section, use the same language used in the user food description in <eaten-food>."
    "- For each item in the text, use an appropriate emoji."
    ""
    ""
    "<reply>"
    "<text>"
    "[emoji] 120 grams dry pasta, ~440 calories (~88g carbs, ~15g protein, ~2g fat)"
    "[emoji] 35 grams tomato, ~6 calories (~1.4 carbs, ~0.3g protein, ~0.1g fat)"
    "..."
    "</text>"
    "<json>"
    "{"
    "   kcal: ...,"
    "   proteins: ...,"
    "   carbos: ...,"
    "   fats: ..."
    "}"
    "</json>"
    "</reply>"
    ""
    "";

/* ======================================== */
/* == 2. Struct Definitions == */
/* ======================================== */

/* Structure to accumulate data received from libcurl */
typedef struct
{
  char *memory;
  size_t size;
} MemoryStruct;

/* Configuration structure */
typedef struct
{
  const char *bot_token;
  const char *deepseek_model;
  const char *deepseek_endpoint;
  int polling_timeout;
  float temperature;
  int max_tokens;
} BotConfig;

/* ======================================== */
/* == 3. Function Declarations == */
/* ======================================== */

/* Callback functions */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp);

/* Memory management */
static MemoryStruct *createMemoryStruct();
static void freeMemoryStruct(MemoryStruct *chunk);

/* HTTP and API functions */
static CURL *initCurl(const char *user_agent);
static char *getDeepSeekResponse(const BotConfig *config, const char *user_message);
static void sendTelegramMessage(const BotConfig *config, long long chat_id, const char *text);
static bool getTelegramUpdates(const BotConfig *config, long long *last_update_id);

/* JSON helpers */
static json_t *createDeepSeekRequestJson(const char *user_message, const char *model_name,
                                         float temperature, int max_tokens);
static char *extractDeepSeekResponse(const char *json_string);

/* Bot initialization */
static BotConfig initBotConfig(const char *bot_token, const char *deepseek_model,
                               const char *deepseek_endpoint);

/* ======================================== */
/* == 4. Memory Management Functions == */
/* ======================================== */

static MemoryStruct *createMemoryStruct()
{
  MemoryStruct *chunk = malloc(sizeof(MemoryStruct));
  if (!chunk)
  {
    fprintf(stderr, "Error: Failed to allocate memory for MemoryStruct\n");
    return NULL;
  }

  chunk->memory = malloc(1);
  if (!chunk->memory)
  {
    fprintf(stderr, "Error: Failed to allocate initial memory buffer\n");
    free(chunk);
    return NULL;
  }

  chunk->size = 0;
  chunk->memory[0] = '\0';
  return chunk;
}

static void freeMemoryStruct(MemoryStruct *chunk)
{
  if (chunk)
  {
    if (chunk->memory)
    {
      free(chunk->memory);
    }
    free(chunk);
  }
}

/* ======================================== */
/* == 5. Callback Function Definitions == */
/* ======================================== */

/* libcurl callback function: writes received data into our MemoryStruct */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  MemoryStruct *mem = (MemoryStruct *)userp;

  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if (ptr == NULL)
  {
    fprintf(stderr, "Error: Not enough memory (realloc failed)\n");
    return 0; /* Signal error to libcurl */
  }

  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = '\0'; /* Add the null terminator */

  return realsize;
}

/* ======================================== */
/* == 6. HTTP and API Functions == */
/* ======================================== */

/* Initialize a CURL handle with common options */
static CURL *initCurl(const char *user_agent)
{
  CURL *curl = curl_easy_init();
  if (curl)
  {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  }
  return curl;
}

/* Function to send a message to a specific chat ID via Telegram */
static void sendTelegramMessage(const BotConfig *config, long long chat_id, const char *text)
{
  CURL *curl_handle;
  CURLcode res;
  MemoryStruct *response_chunk;
  char api_url[MAX_URL_LENGTH];
  char post_data[MAX_POST_DATA_LENGTH];
  char chat_id_str[MAX_CHAT_ID_LENGTH];
  char user_agent[32];
  char *escaped_text = NULL;

  /* Validate inputs */
  if (!config || !config->bot_token || !text)
  {
    fprintf(stderr, "sendTelegramMessage Error: Invalid parameters\n");
    return;
  }

  /* Create memory structure for response */
  response_chunk = createMemoryStruct();
  if (!response_chunk)
  {
    return;
  }

  /* Initialize curl */
  snprintf(user_agent, sizeof(user_agent), "kcal-bot-c/%s", VERSION);
  curl_handle = initCurl(user_agent);
  if (!curl_handle)
  {
    fprintf(stderr, "sendTelegramMessage Error: Could not initialize curl handle\n");
    freeMemoryStruct(response_chunk);
    return;
  }

  /* Prepare URL and data */
  snprintf(api_url, sizeof(api_url), "%s%s/sendMessage", TELEGRAM_API_BASE, config->bot_token);

  /* URL-escape the message text */
  CURL *curl_esc = curl_easy_init();
  if (curl_esc)
  {
    escaped_text = curl_easy_escape(curl_esc, text, 0);
    curl_easy_cleanup(curl_esc);
  }

  if (!escaped_text)
  {
    fprintf(stderr, "sendTelegramMessage Error: Could not URL-escape text\n");
    curl_easy_cleanup(curl_handle);
    freeMemoryStruct(response_chunk);
    return;
  }

  /* Prepare POST data */
  snprintf(chat_id_str, sizeof(chat_id_str), "%lld", chat_id);
  snprintf(post_data, sizeof(post_data), "chat_id=%s&text=%s", chat_id_str, escaped_text);

  /* Set curl options */
  curl_easy_setopt(curl_handle, CURLOPT_URL, api_url);
  curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)response_chunk);

  /* Perform request */
  res = curl_easy_perform(curl_handle);
  if (res != CURLE_OK)
  {
    fprintf(stderr, "sendTelegramMessage: curl_easy_perform() failed: %s\n",
            curl_easy_strerror(res));
  }

  /* Cleanup */
  curl_free(escaped_text);
  curl_easy_cleanup(curl_handle);
  freeMemoryStruct(response_chunk);
}

/* Create DeepSeek request JSON */
static json_t *createDeepSeekRequestJson(const char *user_message, const char *model_name,
                                         float temperature, int max_tokens)
{
  json_t *request_root = json_object();
  if (!request_root)
  {
    fprintf(stderr, "DeepSeek Error: Failed to create root JSON object\n");
    return NULL;
  }

  /* Set model parameters */
  json_object_set_new(request_root, "model", json_string(model_name));
  json_object_set_new(request_root, "temperature", json_real(temperature));
  json_object_set_new(request_root, "max_tokens", json_integer(max_tokens));

  /* Create messages array */
  json_t *messages_array = json_array();
  if (!messages_array)
  {
    json_decref(request_root);
    return NULL;
  }

  /* System message */
  json_t *system_message = json_object();
  if (!system_message)
  {
    json_decref(messages_array);
    json_decref(request_root);
    return NULL;
  }

  json_object_set_new(system_message, "role", json_string("system"));
  json_object_set_new(system_message, "content", json_string(SYSTEM_PROMPT));

  if (json_array_append_new(messages_array, system_message) != 0)
  {
    json_decref(system_message);
    json_decref(messages_array);
    json_decref(request_root);
    return NULL;
  }

  /* User message */
  json_t *user_message_obj = json_object();
  if (!user_message_obj)
  {
    json_decref(messages_array);
    json_decref(request_root);
    return NULL;
  }

  json_object_set_new(user_message_obj, "role", json_string("user"));
  json_object_set_new(user_message_obj, "content", json_string(user_message));

  if (json_array_append_new(messages_array, user_message_obj) != 0)
  {
    json_decref(user_message_obj);
    json_decref(messages_array);
    json_decref(request_root);
    return NULL;
  }

  /* Add messages array to request object */
  if (json_object_set_new(request_root, "messages", messages_array) != 0)
  {
    json_decref(messages_array);
    json_decref(request_root);
    return NULL;
  }

  return request_root;
}

static char *extractDeepSeekResponse(const char *json_string)
{
  json_error_t error;
  json_t *response_root = json_loads(json_string, 0, &error);
  char *response_content = NULL;
  char *reply_content = NULL;

  if (!response_root)
  {
    fprintf(stderr, "DeepSeek Error: Failed to parse response JSON at line %d: %s\n",
            error.line, error.text);
    fprintf(stderr, "DeepSeek Raw Response: %s\n", json_string);
    return NULL;
  }

  /* Try to extract response from DeepSeek's response format */
  json_t *choices_array = json_object_get(response_root, "choices");
  if (!json_is_array(choices_array) || json_array_size(choices_array) == 0)
  {
    fprintf(stderr, "DeepSeek Error: 'choices' array not found or empty in response\n");

    /* Check for error message */
    json_t *error_obj = json_object_get(response_root, "error");
    if (json_is_object(error_obj))
    {
      json_t *message = json_object_get(error_obj, "message");
      if (json_is_string(message))
      {
        fprintf(stderr, "DeepSeek API Error: %s\n", json_string_value(message));
      }
    }

    json_decref(response_root);
    return NULL;
  }

  json_t *first_choice = json_array_get(choices_array, 0);
  if (!json_is_object(first_choice))
  {
    fprintf(stderr, "DeepSeek Error: First choice is not an object\n");
    json_decref(response_root);
    return NULL;
  }

  /* Try different possible paths to find the actual content */
  json_t *message_obj = json_object_get(first_choice, "message");
  if (json_is_object(message_obj))
  {
    /* OpenAI-compatible path */
    json_t *content_json = json_object_get(message_obj, "content");
    if (json_is_string(content_json))
    {
      const char *content_str = json_string_value(content_json);
      response_content = strdup(content_str);
    }
  }
  else
  {
    /* Check if it's directly in the choice object */
    json_t *text_json = json_object_get(first_choice, "text");
    if (json_is_string(text_json))
    {
      const char *text_str = json_string_value(text_json);
      response_content = strdup(text_str);
    }
    else
    {
      fprintf(stderr, "DeepSeek Error: Cannot find content in response\n");
    }
  }

  /* Check for successful extraction of full response */
  if (!response_content)
  {
    fprintf(stderr, "DeepSeek Error: Failed to extract response content\n");
    fprintf(stderr, "DeepSeek Raw Response: %s\n", json_string);
    json_decref(response_root);
    return NULL;
  }

  /* Extract only the <reply> content */
  const char *reply_start = strstr(response_content, "<reply>");
  const char *reply_end = strstr(response_content, "</reply>");

  if (reply_start && reply_end && reply_end > reply_start)
  {
    /* Calculate length of reply content including the tags */
    size_t reply_length = (reply_end - reply_start) + 8; /* +8 for "</reply>" */
    reply_content = (char *)malloc(reply_length + 1);    /* +1 for null terminator */

    if (reply_content)
    {
      /* Copy just the <reply>...</reply> portion */
      strncpy(reply_content, reply_start, reply_length);
      reply_content[reply_length] = '\0'; /* Ensure null termination */

      /* Debug: Print what we found */
      printf("Found <reply> tag content: %s\n", reply_content);

      free(response_content); /* Free the full response */
      json_decref(response_root);
      return reply_content;
    }
  }

  /* If we couldn't extract the reply tag or memory allocation failed, try to create a simple reply */
  fprintf(stderr, "Warning: Could not extract <reply> tag, creating a simple response\n");
  free(response_content);

  /* Create a simple response to avoid showing the thinking process */
  reply_content = strdup("<reply>\n<text>\nSorry, I couldn't analyze your food properly. Please try again with more details.\n</text>\n<json>\n{\"kcal\": 0, \"proteins\": 0, \"carbos\": 0, \"fats\": 0}\n</json>\n</reply>");

  json_decref(response_root);
  return reply_content;
}

/* Function to communicate with DeepSeek API */
static char *getDeepSeekResponse(const BotConfig *config, const char *user_message)
{
  CURL *curl_handle = NULL;
  CURLcode res;
  MemoryStruct *chunk;
  struct curl_slist *headers = NULL;
  char *response_content = NULL;
  char *request_json_str = NULL;
  json_t *request_root = NULL;
  char user_agent[32];

  /* Validate inputs */
  if (!config || !user_message)
  {
    fprintf(stderr, "DeepSeek Error: Invalid parameters (NULL input)\n");
    return NULL;
  }

  /* Create memory structure for response */
  chunk = createMemoryStruct();
  if (!chunk)
  {
    return NULL;
  }

  /* Create the JSON request body */
  request_root = createDeepSeekRequestJson(user_message, config->deepseek_model,
                                           config->temperature, config->max_tokens);
  if (!request_root)
  {
    freeMemoryStruct(chunk);
    return NULL;
  }

  /* Convert JSON to string */
  request_json_str = json_dumps(request_root, JSON_COMPACT);
  json_decref(request_root);

  if (!request_json_str)
  {
    fprintf(stderr, "DeepSeek Error: Failed to dump request JSON to string\n");
    freeMemoryStruct(chunk);
    return NULL;
  }

  /* Debug: Print request */
  printf("DeepSeek Request: %s\n", request_json_str);

  /* Initialize curl for POST request */
  snprintf(user_agent, sizeof(user_agent), "kcal-bot-c/%s", VERSION);
  curl_handle = initCurl(user_agent);
  if (!curl_handle)
  {
    fprintf(stderr, "DeepSeek Error: Failed curl_easy_init\n");
    free(request_json_str);
    freeMemoryStruct(chunk);
    return NULL;
  }

  /* Set DeepSeek URL and options */
  curl_easy_setopt(curl_handle, CURLOPT_URL, config->deepseek_endpoint);
  curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
  curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, request_json_str);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)chunk);

  /* Set headers */
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!headers)
  {
    fprintf(stderr, "DeepSeek Error: Failed curl_slist_append\n");
    free(request_json_str);
    curl_easy_cleanup(curl_handle);
    freeMemoryStruct(chunk);
    return NULL;
  }
  curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

  /* Perform the request */
  res = curl_easy_perform(curl_handle);

  /* Check for curl errors */
  if (res != CURLE_OK)
  {
    fprintf(stderr, "DeepSeek Error: curl_easy_perform() failed: %s\n",
            curl_easy_strerror(res));
    fprintf(stderr, "(Is DeepSeek running at %s?)\n", config->deepseek_endpoint);
  }
  else
  {
    /* Debug: Print raw response */
    printf("DeepSeek Raw Response: %s\n", chunk->memory);

    /* Parse the JSON response */
    response_content = extractDeepSeekResponse(chunk->memory);
  }

  /* Cleanup */
  free(request_json_str);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl_handle);
  freeMemoryStruct(chunk);

  return response_content;
}

/* Function to get updates from Telegram API */
static bool getTelegramUpdates(const BotConfig *config, long long *last_update_id)
{
  CURL *curl_handle = NULL;
  CURLcode res;
  MemoryStruct *chunk;
  char api_url[MAX_URL_LENGTH];
  char user_agent[32];
  bool success = false;

  /* Validate inputs */
  if (!config || !config->bot_token || !last_update_id)
  {
    fprintf(stderr, "getTelegramUpdates Error: Invalid parameters\n");
    return false;
  }

  /* Create memory structure for response */
  chunk = createMemoryStruct();
  if (!chunk)
  {
    return false;
  }

  /* Initialize curl */
  snprintf(user_agent, sizeof(user_agent), "kcal-bot-c/%s", VERSION);
  curl_handle = initCurl(user_agent);
  if (!curl_handle)
  {
    fprintf(stderr, "getTelegramUpdates Error: Failed curl_easy_init\n");
    freeMemoryStruct(chunk);
    return false;
  }

  /* Prepare URL with offset and timeout */
  snprintf(api_url, sizeof(api_url),
           "%s%s/getUpdates?offset=%lld&timeout=%d",
           TELEGRAM_API_BASE, config->bot_token, *last_update_id, config->polling_timeout);

  /* Set curl options */
  curl_easy_setopt(curl_handle, CURLOPT_URL, api_url);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)chunk);

  /* Perform the request */
  res = curl_easy_perform(curl_handle);

  /* Check for curl errors */
  if (res != CURLE_OK)
  {
    fprintf(stderr, "TG Error: getUpdates curl_easy_perform() failed: %s\n",
            curl_easy_strerror(res));
  }
  else
  {
    json_error_t error;
    json_t *root = json_loads(chunk->memory, 0, &error);

    if (!root)
    {
      fprintf(stderr, "TG Error: parsing getUpdates JSON: line %d: %s\n",
              error.line, error.text);
      if (chunk->size > 0)
      {
        fprintf(stderr, "TG Received: %s\n", chunk->memory);
      }
    }
    else
    {
      /* Check if response is successful */
      json_t *ok_status = json_object_get(root, "ok");
      if (json_is_boolean(ok_status) && json_boolean_value(ok_status))
      {
        json_t *result_array = json_object_get(root, "result");
        if (json_is_array(result_array))
        {
          size_t index;
          json_t *update_json;

          json_array_foreach(result_array, index, update_json)
          {
            /* Process update ID */
            json_t *update_id_json = json_object_get(update_json, "update_id");
            if (json_is_integer(update_id_json))
            {
              long long current_update_id = json_integer_value(update_id_json);
              if (current_update_id >= *last_update_id)
              {
                *last_update_id = current_update_id + 1;
              }
            }

            /* Process message */
            json_t *message = json_object_get(update_json, "message");
            if (json_is_object(message))
            {
              /* Get chat ID */
              json_t *chat = json_object_get(message, "chat");
              json_t *chat_id_json = NULL;

              if (json_is_object(chat))
              {
                chat_id_json = json_object_get(chat, "id");
              }

              if (!json_is_integer(chat_id_json))
              {
                continue; /* Skip message if no valid chat_id */
              }

              long long chat_id = json_integer_value(chat_id_json);

              /* Get message text */
              json_t *text_json = json_object_get(message, "text");
              if (json_is_string(text_json))
              {
                const char *text = json_string_value(text_json);
                printf("Received from Chat ID %lld: %s\n", chat_id, text);

                /* Process message with DeepSeek */
                char *llm_response = getDeepSeekResponse(config, text);
                char reply_text[MAX_REPLY_LENGTH];

                if (llm_response != NULL)
                {
                  printf("DeepSeek Response: %s\n", llm_response);
                  snprintf(reply_text, sizeof(reply_text), "%s", llm_response);
                  free(llm_response);
                }
                else
                {
                  printf("Failed to get response from DeepSeek LLM.\n");
                  snprintf(reply_text, sizeof(reply_text),
                           "Sorry, I encountered an error processing your request.");
                }

                /* Send response back to user */
                sendTelegramMessage(config, chat_id, reply_text);
              }
            }
          }

          success = true;
        }
      }
      else
      {
        /* Handle {"ok": false} from getUpdates */
        json_t *description = json_object_get(root, "description");
        const char *error_desc = json_is_string(description) ? json_string_value(description) : "No description";
        fprintf(stderr, "TG Error: Telegram getUpdates API: %s\n", error_desc);

        /* Check for unauthorized error */
        if (description && json_is_string(description) &&
            strstr(json_string_value(description), "Unauthorized"))
        {
          fprintf(stderr, "Please check if your Telegram API token is correct!\n");
        }
      }

      json_decref(root);
    }
  }

  /* Cleanup */
  curl_easy_cleanup(curl_handle);
  freeMemoryStruct(chunk);

  return success;
}

/* ======================================== */
/* == 7. Bot Configuration Functions == */
/* ======================================== */

/* Initialize bot configuration with default values */
static BotConfig initBotConfig(const char *bot_token, const char *deepseek_model,
                               const char *deepseek_endpoint)
{
  BotConfig config;

  config.bot_token = bot_token ? bot_token : "";
  config.deepseek_model = deepseek_model && strlen(deepseek_model) > 0 ? deepseek_model : DEFAULT_DEEPSEEK_MODEL;
  config.deepseek_endpoint = deepseek_endpoint && strlen(deepseek_endpoint) > 0 ? deepseek_endpoint : DEEPSEEK_API_ENDPOINT;
  config.polling_timeout = 60;
  config.temperature = 0.2f; // Lower temperature for more precise calorie estimates
  config.max_tokens = 128;   // Short responses are sufficient for this task

  return config;
}

/* ======================================== */
/* == 8. Main Function == */
/* ======================================== */

int main(int argc, char *argv[])
{
  /* Bot configuration */
  const char *bot_token = "";
  const char *deepseek_model = NULL;
  const char *deepseek_endpoint = NULL;

  /* Check for command-line arguments */
  if (argc > 1)
  {
    deepseek_model = argv[1];
  }

  if (argc > 2)
  {
    deepseek_endpoint = argv[2];
  }

  /* Initialize bot configuration */
  BotConfig config = initBotConfig(bot_token, deepseek_model, deepseek_endpoint);

  /* Validate configuration */
  if (strlen(config.bot_token) == 0)
  {
    fprintf(stderr, "ERROR: Bot token not set!\n");
    return 1;
  }

  /* Initialize libcurl globally */
  if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
  {
    fprintf(stderr, "Error: curl_global_init() failed.\n");
    return 1;
  }

  /* Initialize offset for Telegram updates */
  long long last_update_id = 0;

  /* Print startup info */
  printf("Telegram Calorie Calculator Bot v%s (DeepSeek Edition)\n", VERSION);
  printf("Using DeepSeek model: %s\n", config.deepseek_model);
  printf("DeepSeek API endpoint: %s\n", config.deepseek_endpoint);
  printf("Press Ctrl+C to stop.\n");

  /* Main loop */
  while (1)
  {
    if (!getTelegramUpdates(&config, &last_update_id))
    {
      /* If failed to get updates, wait before retrying */
      sleep(5);
    }
  }

  /* Cleanup (unreachable in current implementation) */
  curl_global_cleanup();
  return 0;
}