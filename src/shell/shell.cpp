#include "shell/shell.hpp"

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace shell {
namespace {

enum class TokenType { Word, Pipe, Input, Output, Append, ErrorOutput, Background };

struct Token {
    TokenType type;
    string text;
};

string quoteForShell(const string& value) {
    string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += '\'';
    return quoted;
}

bool substitute(const string& expression, string& value,
                string& error, const string& workingDirectory) {
    const string script = workingDirectory.empty() ? expression
        : "cd " + quoteForShell(workingDirectory) + " && " + expression;
    const string command = "sh -c " + quoteForShell(script);
    FILE* stream = popen(command.c_str(), "r");
    if (stream == nullptr) {
        error = "could not start command substitution";
        return false;
    }

    char buffer[1024];
    while (const size_t count = fread(buffer, 1, sizeof(buffer), stream)) {
        value.append(buffer, count);
    }
    pclose(stream);
    while (!value.empty() && value.back() == '\n') {
        value.pop_back();
    }
    return true;
}

size_t findBacktickEnd(const string& input, size_t opening) {
    size_t nesting = 1;
    for (size_t i = opening + 1; i < input.size(); ++i) {
        if (input[i] != '`') continue;
        size_t backslashes = 0;
        for (size_t at = i; at > opening + 1 && input[at - 1] == '\\'; --at)
            ++backslashes;
        if (nesting == 1 && backslashes == 0) return i;
        if (nesting > 1 && backslashes == nesting - 1) --nesting;
        else if (backslashes == nesting) ++nesting;
    }
    return string::npos;
}

size_t findCommandSubstitutionEnd(const string& input,
                                       size_t opening) {
    size_t depth = 1;
    char quote = 0;
    for (size_t i = opening + 2; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '$' && i + 1 < input.size() && input[i + 1] == '(' &&
            quote != '\'') {
            const size_t nestedEnd = findCommandSubstitutionEnd(input, i);
            if (nestedEnd == string::npos) return nestedEnd;
            i = nestedEnd;
            continue;
        }
        if (ch == '\\' && quote != '\'') {
            if (i + 1 < input.size()) ++i;
            continue;
        }
        if (ch == '`' && quote != '\'') {
            const size_t end = findBacktickEnd(input, i);
            if (end == string::npos) return end;
            i = end;
            continue;
        }
        if (quote != 0) {
            if (ch == quote) quote = 0;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (ch == '(') {
            ++depth;
        } else if (ch == ')' && --depth == 0) {
            return i;
        }
    }
    return string::npos;
}

ParseState tokenize(const string& input, vector<Token>& tokens,
                    string& error, int lastExitStatus,
                    bool evaluateSubstitutions,
                    const string& workingDirectory) {
    string word;
    bool wordStarted = false;
    char quote = 0;

    auto finishWord = [&] {
        if (wordStarted) {
            tokens.push_back({TokenType::Word, std::move(word)});
            word.clear();
            wordStarted = false;
        }
    };

    for (size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];

        if (ch == '\\' && quote != '\'') {
            if (i + 1 == input.size()) {
                return ParseState::Incomplete;
            }
            const char escaped = input[++i];
            if (quote == '"' && escaped != '$' && escaped != '`' &&
                escaped != '"' && escaped != '\\' && escaped != '\n') {
                word += '\\';
                word += escaped;
                wordStarted = true;
            } else if (escaped != '\n') {
                word += escaped;
                wordStarted = true;
            }
            continue;
        }

        if (ch == '$' && i + 1 < input.size() && input[i + 1] == '(' &&
            quote != '\'') {
            const size_t closing = findCommandSubstitutionEnd(input, i);
            if (closing == string::npos) return ParseState::Incomplete;
            string replacement;
            if (evaluateSubstitutions &&
                !substitute(input.substr(i + 2, closing - i - 2), replacement, error,
                            workingDirectory)) {
                return ParseState::Invalid;
            }
            word += replacement;
            wordStarted = true;
            i = closing;
            continue;
        }

        if (ch == '`' && quote != '\'') {
            const size_t end = findBacktickEnd(input, i);
            if (end == string::npos) return ParseState::Incomplete;
            string expression = input.substr(i + 1, end - i - 1);
            string replacement;
            if (evaluateSubstitutions && !substitute(expression, replacement, error, workingDirectory))
                return ParseState::Invalid;
            word += replacement;
            wordStarted = true;
            i = end;
            continue;
        }

        if (ch == '$' && quote != '\'' && i + 1 < input.size() && input[i + 1] == '?') {
            word += to_string(lastExitStatus);
            wordStarted = true;
            ++i;
            continue;
        }

        if (ch == '$' && quote != '\'' && i + 1 < input.size() &&
            (isalpha(static_cast<unsigned char>(input[i + 1])) || input[i + 1] == '_')) {
            size_t end = i + 2;
            while (end < input.size() && (isalnum(static_cast<unsigned char>(input[end])) || input[end] == '_')) ++end;
            const string name = input.substr(i + 1, end - i - 1);
            const char* value = getenv(name.c_str());
            if (value) word += value;
            wordStarted = true;
            i = end - 1;
            continue;
        }

        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            } else {
                word += ch;
            }
            wordStarted = true;
            continue;
        }

        if (ch == '\'' || ch == '"') {
            quote = ch;
            wordStarted = true;
        } else if (ch == ' ' || ch == '\t' || ch == '\n') {
            finishWord();
        } else if (ch == '|') {
            finishWord();
            tokens.push_back({TokenType::Pipe, "|"});
        } else if (ch == '<') {
            finishWord();
            tokens.push_back({TokenType::Input, "<"});
        } else if (ch == '>') {
            finishWord();
            if (i + 1 < input.size() && input[i + 1] == '>') {
                tokens.push_back({TokenType::Append, ">>"});
                ++i;
            } else {
                tokens.push_back({TokenType::Output, ">"});
            }
        } else if (ch == '2' && i + 1 < input.size() && input[i + 1] == '>') {
            finishWord();
            tokens.push_back({TokenType::ErrorOutput, "2>"});
            ++i;
        } else if (ch == '&') {
            finishWord();
            tokens.push_back({TokenType::Background, "&"});
        } else {
            word += ch;
            wordStarted = true;
        }
    }

    if (quote != 0) {
        return ParseState::Incomplete;
    }
    finishWord();
    return ParseState::Complete;
}

} 

ParseResult parse(const string& input, int lastExitStatus,
                  bool evaluateSubstitutions,
                  const string& workingDirectory) {
    ParseResult result;
    vector<Token> tokens;
    result.state = tokenize(input, tokens, result.error, lastExitStatus,
                            evaluateSubstitutions, workingDirectory);
    if (result.state != ParseState::Complete) {
        return result;
    }
    if (tokens.empty()) {
        return result;
    }

    result.pipeline.commands.emplace_back();
    bool needsRedirectionTarget = false;
    int redirectionFd = -1;
    bool append = false;
    for (const Token& token : tokens) {
        if (needsRedirectionTarget) {
            if (token.type != TokenType::Word) {
                result.state = ParseState::Invalid;
                result.error = "expected a file name after redirection";
                return result;
            }
            result.pipeline.commands.back().redirections.push_back(
                {redirectionFd, token.text, append});
            needsRedirectionTarget = false;
            continue;
        }

        switch (token.type) {
            case TokenType::Word:
                result.pipeline.commands.back().arguments.push_back(token.text);
                break;
            case TokenType::Pipe:
                if (result.pipeline.commands.back().arguments.empty()) {
                    result.state = ParseState::Invalid;
                    result.error = "missing command before pipe";
                    return result;
                }
                result.pipeline.commands.emplace_back();
                break;
            case TokenType::Input:
                redirectionFd = 0;
                append = false;
                needsRedirectionTarget = true;
                break;
            case TokenType::Output:
            case TokenType::Append:
                redirectionFd = 1;
                append = token.type == TokenType::Append;
                needsRedirectionTarget = true;
                break;
            case TokenType::ErrorOutput:
                redirectionFd = 2;
                append = false;
                needsRedirectionTarget = true;
                break;
            case TokenType::Background:
                if (&token != &tokens.back()) {
                    result.state = ParseState::Invalid;
                    result.error = "'&' must be at the end of a command";
                    return result;
                }
                result.pipeline.background = true;
                break;
        }
    }

    if (needsRedirectionTarget) {
        result.state = ParseState::Incomplete;
        return result;
    }
    if (result.pipeline.commands.back().arguments.empty()) {
        result.state = ParseState::Incomplete;
    }
    return result;
}

} 
