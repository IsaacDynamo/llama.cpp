// A basic application simulating a server with multiple clients.
// The clients submit requests to the server and they are processed in parallel.

#include "arg.h"
#include "common.h"
#include "sampling.h"
#include "log.h"
#include "llama.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <ctime>
#include <algorithm>

// trim whitespace from the beginning and end of a string
static std::string trim(const std::string & str) {
    size_t start = 0;
    size_t end = str.size();

    while (start < end && isspace(str[start])) {
        start += 1;
    }

    while (end > start && isspace(str[end - 1])) {
        end -= 1;
    }

    return str.substr(start, end - start);
}

static std::string k_system =
R"(Transcript of a never ending dialog, where the User interacts with an Assistant.
The Assistant is helpful, kind, honest, good at writing, and never fails to answer the User's requests immediately and with precision.

User:
Recommend a nice restaurant in the area.
Assistant:
I recommend the restaurant "The Golden Duck". It is a 5 star restaurant with a great view of the city. The food is delicious and the service is excellent. The prices are reasonable and the portions are generous. The restaurant is located at 123 Main Street, New York, NY 10001. The phone number is (212) 555-1234. The hours are Monday through Friday from 11:00 am to 10:00 pm. The restaurant is closed on Saturdays and Sundays.
User:
Who is Richard Feynman?
Assistant:
Richard Feynman was an American physicist who is best known for his work in quantum mechanics and particle physics. He was awarded the Nobel Prize in Physics in 1965 for his contributions to the development of quantum electrodynamics. He was a popular lecturer and author, and he wrote several books, including "Surely You're Joking, Mr. Feynman!" and "What Do You Care What Other People Think?".
)";

static std::vector<std::string> k_questions = {
    "What is the tallest mountain in the world?",
    "Who was the first person to win two Nobel Prizes?",
    "Which country invented paper?",
    "What organ is primarily responsible for pumping blood throughout the body?",
    "Which planet is known for its prominent ring system?",
    "Who directed the movie 'Inception'?",
    "What is the freezing point of water in Fahrenheit?",
    "Which animal is known to have the longest lifespan?",
    "What language has the most native speakers worldwide?",
    "What is the capital city of Canada?",
    "Who is credited with inventing the World Wide Web?",
    "Which metal is liquid at room temperature?",
    "What is the term for an animal that eats both plants and meat?",
    "Who painted 'The Starry Night'?",
    "What gas do humans exhale that plants use for photosynthesis?",
    "What year did World War II end?",
    "Which continent has the most countries?",
    "Who wrote the novel 'Frankenstein'?",
    "What does DNA stand for?",
    "What is the main ingredient in traditional Japanese miso soup?"
};

static std::vector<std::string> k_answers = {
    "The tallest mountain in the world is Mount Everest.",
    "Marie Curie was the first person to win two Nobel Prizes.",
    "Paper was invented in China.",
    "The heart is the organ responsible for pumping blood.",
    "Saturn is known for its prominent ring system.",
    "Christopher Nolan directed the movie 'Inception'.",
    "The freezing point of water in Fahrenheit is 32°F.",
    "The bowhead whale is known to have the longest lifespan among mammals.",
    "Mandarin Chinese has the most native speakers in the world.",
    "The capital city of Canada is Ottawa.",
    "Tim Berners-Lee is credited with inventing the World Wide Web.",
    "Mercury is the metal that is liquid at room temperature.",
    "An animal that eats both plants and meat is called an omnivore.",
    "'The Starry Night' was painted by Vincent van Gogh.",
    "Humans exhale carbon dioxide, which plants use in photosynthesis.",
    "World War II ended in 1945.",
    "Africa is the continent with the most countries.",
    "The novel 'Frankenstein' was written by Mary Shelley.",
    "DNA stands for Deoxyribonucleic Acid.",
    "The main ingredient in traditional Japanese miso soup is fermented soybean paste."
};

static std::vector<std::string> k_prompts = {
    "What is the meaning of life?",
    "Tell me an interesting fact about llamas.",
    "What is the best way to cook a steak?",
    "Are you familiar with the Special Theory of Relativity and can you explain it to me?",
    "Recommend some interesting books to read.",
    "What is the best way to learn a new language?",
    "How to get a job at Google?",
    "If you could have any superpower, what would it be?",
    "I want to learn how to play the piano. What would be the best way to do it?",
};

struct client {
    ~client() {
        if (smpl) {
            common_sampler_free(smpl);
        }
    }

    int32_t id = 0;

    llama_seq_id seq_id = -1;

    llama_token sampled;

    int64_t t_start_prompt;
    int64_t t_start_gen;

    int32_t n_past    = 0;
    int32_t n_prompt  = 0;
    int32_t n_decoded = 0;
    int32_t i_batch   = -1;

    std::string input;
    std::string prompt;
    std::string response;

    struct common_sampler * smpl = nullptr;
};

static void print_date_time() {
    std::time_t current_time = std::time(nullptr);
    std::tm* local_time = std::localtime(&current_time);
    char buffer[80];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", local_time);

    LOG_INF("\n");
    LOG_INF("\033[35mrun parameters as of %s\033[0m\n", buffer);
    LOG_INF("\n");
}

// Define a split string function to ...
static std::vector<std::string> split_string(const std::string& input, char delimiter) {
    std::vector<std::string> tokens;
    std::istringstream stream(input);
    std::string token;
    while (std::getline(stream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

int main(int argc, char ** argv) {
    srand(1234);

    common_params params;

    params.n_predict = 128;
    params.n_junk = 1;

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_PARALLEL)) {
        return 1;
    }

    common_init();

    // number of simultaneous "clients" to simulate
    const int32_t n_clients = params.n_parallel;

    // dedicate one sequence to the system prompt
    params.n_parallel += 1;

    // requests to simulate
    const int32_t n_seq = params.n_sequences;

    // insert new requests as soon as the previous one is done
    const bool cont_batching = params.cont_batching;

    // is the system prompt shared in the cache
    const bool is_sp_shared = params.is_pp_shared;

    // extra text to insert in each client's prompt in order to make it larger
    const int32_t n_junk = std::max(1, params.n_junk);

    // signed seed, use negative values to indicate different seeds for the different clients
    const int32_t & sseed = params.sampling.seed;

    // init llama.cpp
    llama_backend_init();
    llama_numa_init(params.numa);

    // load the target model
    common_init_result llama_init = common_init_from_params(params);

    llama_model * model = llama_init.model.get();
    llama_context * ctx = llama_init.context.get();

    auto * mem = llama_get_memory(ctx);

    const llama_vocab * vocab = llama_model_get_vocab(model);

    // load the prompts from an external file if there are any
    if (params.prompt.empty()) {
        LOG_INF("\033[32mNo new questions so proceed with build-in defaults.\033[0m\n");
    } else {
        // Output each line of the input params.prompts vector and copy to k_prompts
        int index = 0;
        LOG_INF("\033[32mNow printing the external prompt file %s\033[0m\n\n", params.prompt_file.c_str());

        std::vector<std::string> prompts = split_string(params.prompt, '\n');
        for (const auto& prompt : prompts) {
            k_prompts.resize(index + 1);
            k_prompts[index] = prompt;
            index++;
            LOG_INF("%3d prompt: %s\n", index, prompt.c_str());
        }
    }

    LOG_INF("\n\n");

    const int n_ctx = llama_n_ctx(ctx);

    if (sseed >= 0) {
        LOG_INF("%s: initializing all samplers with the same RNG seed: %d (use a negative seed to have different seeds)\n", __func__, sseed);
    } else {
        LOG_INF("%s: initializing samplers with different RNG seeds, starting from %d\n", __func__, sseed);
    }

    std::vector<client> clients(n_clients);
    for (size_t i = 0; i < clients.size(); ++i) {
        auto & client = clients[i];
        client.id = i;
        client.smpl = common_sampler_init(model, params.sampling);

        if (sseed < 0) {
            params.sampling.seed--;
        }
    }

    std::vector<llama_token> tokens_system;

    tokens_system = common_tokenize(ctx, k_system, true);
    const int32_t n_tokens_system = tokens_system.size();

    llama_seq_id g_seq_id = 0;

    // the max batch size is as large as the context to handle cases where we get very long input prompt from multiple
    // users. regardless of the size, the main loop will chunk the batch into a maximum of params.n_batch tokens at a time
    llama_batch batch = llama_batch_init(n_ctx, 0, 1);

    int32_t n_total_prompt = 0;
    int32_t n_total_gen    = 0;
    int32_t n_cache_miss   = 0;

    const auto t_main_start = ggml_time_us();

    LOG_INF("%s: Simulating parallel requests from clients:\n", __func__);
    LOG_INF("%s: n_parallel = %d, n_sequences = %d, cont_batching = %d, system tokens = %d\n", __func__, n_clients, n_seq, cont_batching, n_tokens_system);
    LOG_INF("\n");

    if (is_sp_shared) {
        LOG_INF("%s: Evaluating the system prompt ...\n", __func__);

        for (int32_t i = 0; i < n_tokens_system; ++i) {
            common_batch_add(batch, tokens_system[i], i, { 0 }, false);
        }

        if (llama_decode(ctx, batch) != 0) {
            LOG_ERR("%s: llama_decode() failed\n", __func__);
            return 1;
        }

        // assign the system KV cache to all parallel sequences
        for (int32_t i = 1; i <= n_clients; ++i) {
            llama_memory_seq_cp(mem, 0, i, -1, -1);
        }

        LOG_INF("\n");
    }

    LOG_INF("Processing requests ...\n\n");

    while (true) {
        common_batch_clear(batch);

        // decode any currently ongoing sequences
        for (auto & client : clients) {
            if (client.seq_id == -1) {
                continue;
            }

            client.i_batch = batch.n_tokens;

            common_batch_add(batch, client.sampled, client.n_past++, { client.id + 1 }, true);

            client.n_decoded += 1;
        }

        if (batch.n_tokens == 0) {
            // all sequences have ended - clear the entire KV cache
            for (int i = 1; i <= n_clients; ++i) {
                llama_memory_seq_rm(mem, i, -1, -1);
                // but keep the system prompt
                llama_memory_seq_cp(mem, 0, i, -1, -1);
            }

            LOG_INF("%s: clearing the KV cache\n", __func__);
        }

        // insert new sequences for decoding
        if (cont_batching || batch.n_tokens == 0) {
            for (auto & client : clients) {
                if (client.seq_id == -1 && g_seq_id < n_seq) {
                    client.seq_id = g_seq_id;

                    client.t_start_prompt = ggml_time_us();
                    client.t_start_gen    = 0;

                    client.input    = k_prompts[rand() % k_prompts.size()];
                    client.response = "";

                    // construct the prompt:
                    // [system prompt] + [junk] + [user prompt]
                    client.n_past = 0;
                    client.prompt = "";
                    if (is_sp_shared) {
                        client.n_past = n_tokens_system;
                    } else {
                        client.prompt += k_system;
                    }

                    const int n_junk_cur = rand() % n_junk;

                    for (int i = 0; i < n_junk_cur; ++i) {
                        const int r = rand() % k_questions.size();
                        client.prompt += "User:\n" + k_questions[r] + "\nAssistant:\n " + k_answers[r] + "\n";
                    }
                    client.prompt += "User:\n" + client.input + "\nAssistant:\n";

                    common_sampler_reset(client.smpl);

                    // do not prepend BOS because we have a system prompt!
                    std::vector<llama_token> tokens_prompt;
                    tokens_prompt = common_tokenize(ctx, client.prompt, false);

                    for (size_t i = 0; i < tokens_prompt.size(); ++i) {
                        common_batch_add(batch, tokens_prompt[i], client.n_past++, { client.id + 1 }, false);
                    }

                    // extract the logits only for the last token
                    if (batch.n_tokens > 0) {
                        batch.logits[batch.n_tokens - 1] = true;
                    }

                    client.n_prompt  = tokens_prompt.size();
                    client.n_decoded = 0;
                    client.i_batch   = batch.n_tokens - 1;

                    LOG_INF("\033[31mClient %3d, seq %4d, junk = %4d, prompt = %d, started decoding ...\033[0m\n", client.id, client.seq_id, n_junk_cur, client.n_prompt);

                    g_seq_id += 1;

                    // insert new requests one-by-one
                    //if (cont_batching) {
                    //    break;
                    //}
                }
            }
        }

        if (batch.n_tokens == 0) {
            break;
        }

        // process in chunks of params.n_batch
        int32_t n_batch = params.n_batch;

        int32_t i_next = 0;

        for (int32_t i = 0; i < batch.n_tokens; i = i_next) {
            // experiment: process in powers of 2
            //if (i + n_batch > (int32_t) batch.n_tokens && n_batch > 32) {
            //    n_batch /= 2;
            //    i -= n_batch;
            //    continue;
            //}

            const int32_t n_tokens = std::min(n_batch, batch.n_tokens - i);

            llama_batch batch_view = {
                n_tokens,
                batch.token    + i,
                nullptr,
                batch.pos      + i,
                batch.n_seq_id + i,
                batch.seq_id   + i,
                batch.logits   + i,
            };

            const int ret = llama_decode(ctx, batch_view);
            if (ret != 0) {
                if (n_batch == 1 || ret < 0) {
                    // if you get here, it means the KV cache is full - try increasing it via the context size
                    LOG_ERR("%s : failed to decode the batch, n_batch = %d, ret = %d\n", __func__, n_batch, ret);
                    return 1;
                }

                LOG_WRN("%s : failed to decode the batch, retrying with n_batch = %d\n", __func__, n_batch / 2);

                n_cache_miss += 1;

                // retry with half the batch size to try to find a free slot in the KV cache
                n_batch /= 2;

                continue;
            }

            LOG_DBG("%s : decoded batch of %d tokens\n", __func__, n_tokens);

            // move the head of the batch forward with the number of tokens we just processed
            i_next = i + n_tokens;

            // on successful decode, restore the original batch size
            n_batch = params.n_batch;

            for (auto & client : clients) {
                if (client.i_batch < (int) i || client.i_batch >= (int) (i + n_tokens)) {
                    continue;
                }

                //printf("client %d, seq %d, token %d, pos %d, batch %d\n",
                //        client.id, client.seq_id, client.sampled, client.n_decoded, client.i_batch);

                const llama_token id = common_sampler_sample(client.smpl, ctx, client.i_batch - i);

                common_sampler_accept(client.smpl, id, true);

                if (client.n_decoded == 1) {
                    // start measuring generation time after the first token to make sure all concurrent clients
                    // have their prompt already processed
                    client.t_start_gen = ggml_time_us();
                }

                const std::string token_str = common_token_to_piece(ctx, id);

                client.response += token_str;
                client.sampled = id;

                //printf("client %d, seq %d, token %d, pos %d, batch %d: %s\n",
                //        client.id, client.seq_id, id, client.n_decoded, client.i_batch, token_str.c_str());

                if (client.n_decoded > 2 &&
                    (llama_vocab_is_eog(vocab, id) ||
                     (params.n_predict > 0 && client.n_decoded >= params.n_predict) ||
                     client.response.find("User:") != std::string::npos)) {
                    // basic reverse prompt
                    const size_t pos = client.response.find("User:");
                    if (pos != std::string::npos) {
                        client.response = client.response.substr(0, pos);
                    }

                    // delete only the generated part of the sequence, i.e. keep the system prompt in the cache
                    llama_memory_seq_rm(mem,    client.id + 1, -1, -1);
                    llama_memory_seq_cp(mem, 0, client.id + 1, -1, -1);

                    const auto t_main_end = ggml_time_us();

                    LOG_INF("\033[31mClient %3d, seq %3d/%3d, prompt %4d t, response %4d t, time %5.2f s, speed %5.2f t/s, cache miss %d \033[0m \n\nInput:    %s\n\033[35mResponse: %s\033[0m\n\n",
                            client.id, client.seq_id, n_seq, client.n_prompt, client.n_decoded,
                            (t_main_end - client.t_start_prompt) / 1e6,
                            (double) (client.n_prompt + client.n_decoded) / (t_main_end - client.t_start_prompt) * 1e6,
                            n_cache_miss,
                            ::trim(client.input).c_str(),
                            ::trim(client.response).c_str());

                    n_total_prompt += client.n_prompt;
                    n_total_gen    += client.n_decoded;

                    client.seq_id = -1;
                }

                client.i_batch = -1;
            }
        }
    }

    const auto t_main_end = ggml_time_us();

    print_date_time();

    LOG_INF("%s: n_parallel = %d, n_sequences = %d, cont_batching = %d, system tokens = %d\n", __func__, n_clients, n_seq, cont_batching, n_tokens_system);
    if (params.prompt_file.empty()) {
        params.prompt_file = "used built-in defaults";
    }
    LOG_INF("External prompt file: \033[32m%s\033[0m\n", params.prompt_file.c_str());
    LOG_INF("Model and path used:  \033[32m%s\033[0m\n\n", params.model.path.c_str());

    LOG_INF("Total prompt tokens: %6d, speed: %5.2f t/s\n", n_total_prompt, (double) (n_total_prompt              ) / (t_main_end - t_main_start) * 1e6);
    LOG_INF("Total gen tokens:    %6d, speed: %5.2f t/s\n", n_total_gen,    (double) (n_total_gen                 ) / (t_main_end - t_main_start) * 1e6);
    LOG_INF("Total speed (AVG):   %6s  speed: %5.2f t/s\n", "",             (double) (n_total_prompt + n_total_gen) / (t_main_end - t_main_start) * 1e6);
    LOG_INF("Cache misses:        %6d\n", n_cache_miss);

    LOG_INF("\n");

    // TODO: print sampling/grammar timings for all clients
    llama_perf_context_print(ctx);

    llama_batch_free(batch);

    llama_backend_free();

    LOG("\n\n");

    return 0;
}
