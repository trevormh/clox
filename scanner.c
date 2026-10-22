#include <stdio.h>
#include <string.h>

#include "common.h"
#include "scanner.h"

/**
 * As the scanner goes through the code it tracks how far its gone
 * start marks the beginning of the current lexeme, and current points to the character
 * Ex:
 *  print bacon;0\
 *        ^  ^
 * - As we're scanning the lexeme "bacon" start would b on the "b"
 * - and when we reach "o" that's where current would be.
 * - The character we most recently consumed was "c"
 */
typedef struct {
    const char* start;
    const char* current;
    int line;
} Scanner;

Scanner scanner;

void initScanner(const char* source) {
    scanner.start = source;
    scanner.current = source;
    scanner.line = 1;
}

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';  // identifiers can be snake_case
}

static bool isDigit(char c) {
    // Characters in C are stored as ASCII numeric codes,
    // which range from 48-57 for numbers.
    return c >= '0' && c <= '9';
}

static bool isAtEnd() {
    // Our source code is 1 big string.
    // When the null termination charcter is encountered we're at the end.
    return *scanner.current == '\0';
}

// read the next character from source code
static char advance() {
    scanner.current++;
    return scanner.current[-1];
}

// Check current caracter but don't increment position
static char peek() {
    return *scanner.current;
}

/**
 * Helper function for finding comments.
 * Since we don't consume the first / in a comment and increment,
 * we need to find the next one after that.
 */
static char peekNext() {
    if (isAtEnd()) return '\0';
    return scanner.current[1];
}

/**
 * Provide an expected character and return true if we find it, false if we don't.
 * Bump the current position if we find a match.
 */
static bool match(char expected) {
    if (isAtEnd()) return false;
    if (*scanner.current != expected) return false;
    scanner.current++;
    return true;
}

static Token makeToken(TokenType type) {
    Token token;
    token.type = type;
    token.start = scanner.start;
    token.length = (int)(scanner.current - scanner.start);
    token.line = scanner.line;
    return token;
}

static Token errorToken(const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = scanner.line;
    return token;
}

static void skipWhitespace() {
    for (;;) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\r':  // carriage return
            case '\t':  // tab
                advance();
                break;
            case '\n':
                scanner.line++;
                advance();
                break;
            // check for comments
            case '/':
                if (peekNext() == '/') {
                    /**
                     * A comment goes until the end of the line.
                     * Check for the newline but don't consume it so the newline
                     * will be the current character on the next iteration of this loop,
                     * which has it's own case to handle it.
                     */
                    while (peek() != '\n' && !isAtEnd()) advance();
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static TokenType checkKeyword(int start, int length, const char* rest, TokenType type) {
    /**
     * Check if the length of the current lexeme matches a reserved word's length,
     * i.e. actual_token_length == expected_keyword_length, and that the
     * characters match too.
     * 
     * memcmp will match on substrings, so we must check the length first.
     * If our current lexeme is "format" and we're matching against "for",
     * memcmp would return a match with "for".
     * It's faster than strcmp because we're operating on raw memory.
     * 
     * Ex: memcmp("f", "for", 3) would result in match
     * - the first "f" arg is the start of our lexeme "format"
     * - the middle arg of "for" would be "or", which is what we want to match
     * - the final arg 3 is how far to traverse out from the "f"
     */
    if (scanner.current - scanner.start == start + length && memcmp(scanner.start + start, rest, length) == 0) {
        return type;
    }

    return TOKEN_IDENTIFIER;
}

/**
 * Helper function to determine if a lexeme is an identifier.
 * Basically acts as a trie.
 */
static TokenType identifierType() {
    /**
     * Args for checkKeyword are:
     * start - starting offset of the lexeme to match. Ex: "return", would point to r
     * end - how far of an offset to check. i.e. how long is the keyword after the first char. Ex: class = 4
     * rest - remaining part of the word we want to check. Ex: rest for "and" would be "nd"
     * type - the type of token to use if we find a match
     */
    switch (scanner.start[0]) {
        case 'a': return checkKeyword(1, 2, "nd", TOKEN_AND);
        case 'c': return checkKeyword(1, 4, "lass", TOKEN_CLASS);
        case 'e': return checkKeyword(1, 3, "lse", TOKEN_ELSE);
        case 'f':
            if (scanner.current - scanner.start > 1) {
                // false, for, fun
                switch (scanner.start[1]) {
                    case 'a': return checkKeyword(2, 3, "lse", TOKEN_FALSE);
                    case 'o': return checkKeyword(2, 1, "r", TOKEN_FOR);
                    case 'u': return checkKeyword(2, 1, "n", TOKEN_FUN);
                }
            }
            break;
        case 'i': return checkKeyword(1, 1, "f", TOKEN_IF);
        case 'n': return checkKeyword(1, 2, "il", TOKEN_NIL);
        case 'o': return checkKeyword(1, 1, "r", TOKEN_OR);
        case 'p': return checkKeyword(1, 4, "rint", TOKEN_PRINT);
        case 'r': return checkKeyword(1, 5, "eturn", TOKEN_RETURN);
        case 's': return checkKeyword(1, 4, "uper", TOKEN_SUPER);
        case 't':
            if (scanner.current - scanner.start > 1) {
                // this, true
                switch (scanner.start[1]) {
                    case 'h': return checkKeyword(2, 2, "is", TOKEN_THIS);
                    case 'r': return checkKeyword(2, 2, "ue", TOKEN_TRUE);
                }
            }
            break;
        case 'v': return checkKeyword(1, 2, "ar", TOKEN_VAR);
        case 'w': return checkKeyword(1, 4, "hile", TOKEN_WHILE);
    }
    return TOKEN_IDENTIFIER;
}

// Helper to identify variable names, class names, etc...
static Token identifier() {
    // after the first letter we allow digits too
    while (isAlpha(peek()) || isDigit(peek())) {
        advance();
    }
    return makeToken(identifierType());
}

/**
 * Helper function to create a token for numbers
 */
static Token number() {
    while (isDigit(peek())) advance();

    // Look for a fractional part.
    if (peek() == '.' && isDigit(peekNext())) {
        // Consume the ".".
        advance();

        while (isDigit(peek())) advance();
    }

    return makeToken(TOKEN_NUMBER);
}

/**
 * Helper function to consume strings.
 *
 * Instead of "extracting" the string text and storing it somewhere
 * we only check where the string starts (set by the call to the scanToken fucntion)
 * and ends (closing quote).
 * A token is created using these positions, which the compiler will convert when needed.
 */
static Token string() {
    // keep
    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n') scanner.line++;
        advance();
    }

    if (isAtEnd()) return errorToken("Unterminated string.");

    // The closing quote.
    advance();
    return makeToken(TOKEN_STRING);
}

Token scanToken() {
    skipWhitespace();  // advance past any leading whitespace

    /**
     * Each call to this function scans a complete token, so we know
     * we're at the beginning of a new token when this is called.
     * Update the starting point to where we left off.
     */
    scanner.start = scanner.current;

    if (isAtEnd()) return makeToken(TOKEN_EOF);

    char c = advance();
    if (isAlpha(c)) return identifier();
    if (isDigit(c)) return number();

    switch (c) {
        case '(': return makeToken(TOKEN_LEFT_PAREN);
        case ')': return makeToken(TOKEN_RIGHT_PAREN);
        case '{': return makeToken(TOKEN_LEFT_BRACE);
        case '}': return makeToken(TOKEN_RIGHT_BRACE);
        case ';': return makeToken(TOKEN_SEMICOLON);
        case ',': return makeToken(TOKEN_COMMA);
        case '.': return makeToken(TOKEN_DOT);
        case '-': return makeToken(TOKEN_MINUS);
        case '+': return makeToken(TOKEN_PLUS);
        case '/': return makeToken(TOKEN_SLASH);
        case '*': return makeToken(TOKEN_STAR);
        // 1 and 2 character punctuations like != and <=
        // We don't know if we're gonna get ! or !=
        case '!':
            return makeToken(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
        case '=':
            return makeToken(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '<':
            return makeToken(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>':
            return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        // match strings
        case '"': return string();
    }

    // If we haven't found a token by now that means we're at
    // a character the scanner can't recognize.
    return errorToken("Unexpected character.");
}