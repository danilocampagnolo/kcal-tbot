# kcal-tbot (DeepSeek) - DIDACTICAL PURPOSE ONLY
# This is a didactical project.

**Version:** 0.0.1

A Telegram bot designed to estimate the calorie and macronutrient content of meals described in natural language messages, using a DeepSeek LLM model for analysis.

## Features

* **Telegram Bot Integration:** Interacts with users via the Telegram Bot API.
* **Natural Language Input:** Users can describe their meals in plain text messages.
* **LLM-Powered Analysis:** Leverages a DeepSeek language model (configurable) to:
    * Estimate ingredients and quantities from the user's description.
    * Split foods into components (e.g., pasta with sauce).
    * Calculate estimated calories and macros (carbohydrates, proteins, fats) based on internal logic and potentially user location context.
* **Structured Output:** Responds with a breakdown of estimated calories/macros per item and a JSON summary of the totals.
* **Configurable:** Allows setting the DeepSeek model, API endpoint, temperature, and max tokens.

## Dependencies

* **libcurl:** For making HTTP requests to the Telegram and DeepSeek APIs.
* **jansson:** For parsing and creating JSON data.

## Configuration

The bot requires the following configurations:

1.  **Telegram Bot Token:** Needs to be set within the `deepseek-kcal-bot.c` source code (currently hardcoded as an empty string, you **must** replace this).
2.  **DeepSeek API Endpoint:** The URL of your running DeepSeek (or compatible) LLM API service. Defaults to `http://localhost:11434/v1/chat/completions` but can be overridden via command-line argument.
3.  **DeepSeek Model Name:** The specific model to use (e.g., `deepseek-r1:14b`). Defaults to `deepseek-r1:14b` but can be overridden via command-line argument.

## Building

You need `gcc` and the development libraries for `libcurl` and `jansson`. Compile the bot using:

```bash
gcc -o deepseek-kcal-bot deepseek-kcal-bot.c $(pkg-config --cflags --libs libcurl jansson)