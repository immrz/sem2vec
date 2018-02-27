//  Copyright 2013 Google Inc. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

// ./word2vec -train /data/disk1/private/nyl/copus2.txt -output vectors12.bin -cbow 0 -size 200 -window 8 -negative 25 -hs 0 -sample 1e-4 -threads 30 -binary 1 -iter 1 -read-vocab ../data2/ReadVocab2700000000 -read-meaning ../ReadMeaning -read-sense ../data2/ReadSenseWord2700000000 -min-count 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>

#define MAX_STRING 100
#define EXP_TABLE_SIZE 1000
#define EXP_FROM_ZERO_FORE 1000
#define MAX_EXP 6
#define MAX_SENTENCE_LENGTH 1000
#define MAX_CODE_LENGTH 40

const int vocab_hash_size = 10000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary
const int meaning_hash_size = 10000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary

// Birthday of an important girl. 
// Thanks to such a fortunate random seed, I get the satisfying results.
unsigned long long next_random = 19960322;

typedef float real;                    // Precision of float numbers

real pre_exp[EXP_TABLE_SIZE]; // calc exp previously

struct vocab_word { // record information of a word
	long long cn; // number of occurrence in train set
	int *point;
	char *word, *code, codelen;
};

real vectorDot(real *a, real *b, int l) {
	int i;
	real dot = 0;
	for (i = 0; i < l; ++i)
		dot += a[i] * b[i];
	return dot;
}

char train_file[MAX_STRING], output_file[MAX_STRING], checkpoint[MAX_STRING];
char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING], read_meaning_file[MAX_STRING], read_sense_file[MAX_STRING];
char read_semantic_proj[MAX_STRING]; // from which file to read the semantic projections
char read_proj_fast[MAX_STRING];
struct vocab_word *vocab;
int binary = 0, cbow = 1, debug_mode = 2, window = 5, min_count = 5, num_threads = 12, min_reduce = 1;
int *vocab_hash;
long long vocab_max_size = 1000, vocab_size = 0, layer1_size = 100, semantic_num = 1000;
long long train_words = 0, word_count_actual = 0, iter = 5, file_size = 0, classes = 0;
real alpha = 0.025, starting_alpha, sample = 1e-3;
real *syn1neg, *expTable;
clock_t start;

real *syn0; // the word embeddings. (vocab_size * layer1_size)
real *syn_sem; // the semantic vectors. (semantic_num * layer1_size)

real *proj; // the projection of words to the semantic bases. (vocab_size * semantic_num)
int *in_list; // whether the words are in the list vocabulary.

int hs = 0, negative = 5;
const int table_size = 1e8;
int *table;

void InitUnigramTable() {
	int a, i;
	double train_words_pow = 0;
	double d1, power = 0.75;
	table = (int *)malloc(table_size * sizeof(int));
	for (a = 0; a < vocab_size; a++) train_words_pow += pow(vocab[a].cn, power);
	i = 0;
	d1 = pow(vocab[i].cn, power) / train_words_pow;
	for (a = 0; a < table_size; a++) {
		table[a] = i;
		if (a / (double)table_size > d1) {
			i++;
			d1 += pow(vocab[i].cn, power) / train_words_pow;
		}
		if (i >= vocab_size) i = vocab_size - 1;
	}
}

int ReadVocabInt(FILE *fin) {
	int a = 0, ch, sub = '0';
	while (!feof(fin)) {
		ch = fgetc(fin);
		if ((ch == ' ') || (ch == '\n'))
			break;
		a = a * 10 + ch - sub;
	}
	return a;
}

void ReadVocabWord(char *word, FILE *fin) {
	int a = 0, ch;
	while (!feof(fin)) {
		ch = fgetc(fin);
		if ((ch == ' ') || (ch == '\n'))
			break;
		word[a] = ch;
		++a;
		if (a >= MAX_STRING - 1)
			--a;
	}
	word[a] = 0;
}

// Reads a single word from a file, assuming space + tab + EOL to be word boundaries
void ReadWord(char *word, FILE *fin) {
	int a = 0, ch;
	while (!feof(fin)) {
		ch = fgetc(fin);
		if (ch == 13) continue;
		if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
			if (a > 0) {
				if (ch == '\n') ungetc(ch, fin);
				break;
			}
			if (ch == '\n') {
				strcpy(word, (char *)"</s>");
				return;
			}
			else continue;
		}
		word[a] = ch;
		a++;
		if (a >= MAX_STRING - 1) a--;   // Truncate too long words
	}
	word[a] = 0;
}

// Reads a single word from a file, assuming space + tab + EOL to be word boundaries
void ReadMeaningWord(char *word, FILE *fin) {
	int a = 0, ch;
	while (!feof(fin)) {
		ch = fgetc(fin);
		if ((ch == ' ') || (ch == '\n'))
			break;
		word[a] = ch;
		++a;
		if (a >= MAX_STRING - 1)
			--a;
	}
	word[a] = 0;
}

// Returns hash value of a word
int GetWordHash(char *word) {
	unsigned long long a, hash = 0;
	for (a = 0; a < strlen(word); a++) hash = hash * 257 + word[a];
	hash = hash % vocab_hash_size;
	return hash;
}

// Returns position of a word in the vocabulary; if the word is not found, returns -1
int SearchVocab(char *word) {
	unsigned int hash = GetWordHash(word);
	while (1) {
		if (vocab_hash[hash] == -1) return -1;
		if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];
		hash = (hash + 1) % vocab_hash_size;
	}
	return -1;
}

// Reads a word and returns its index in the vocabulary
int ReadWordIndex(FILE *fin) {
	char word[MAX_STRING];
	ReadWord(word, fin);
	if (feof(fin)) return -1;
	return SearchVocab(word);
}

// Adds a word to the vocabulary
int AddWordToVocab(char *word) {
	unsigned int hash, length = strlen(word) + 1;
	if (length > MAX_STRING) length = MAX_STRING;
	vocab[vocab_size].word = (char *)calloc(length, sizeof(char));
	strcpy(vocab[vocab_size].word, word);
	vocab[vocab_size].cn = 0;
	vocab_size++;
	// Reallocate memory if needed
	if (vocab_size + 2 >= vocab_max_size) {
		vocab_max_size += 1000;
		vocab = (struct vocab_word *)realloc(vocab, vocab_max_size * sizeof(struct vocab_word));
	}
	hash = GetWordHash(word);
	while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
	vocab_hash[hash] = vocab_size - 1;
	return vocab_size - 1;
}

// Used later for sorting by word counts
int VocabCompare(const void *a, const void *b) {
	return ((struct vocab_word *)b)->cn - ((struct vocab_word *)a)->cn;
}

// Sorts the vocabulary by frequency using word counts
void SortVocab() {
	int a, size;
	unsigned int hash;
	// Sort the vocabulary and keep </s> at the first position
	qsort(&vocab[0], vocab_size - 1, sizeof(struct vocab_word), VocabCompare);
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	size = vocab_size;
	train_words = 0;
	for (a = 0; a < size; a++) {
		// Words occuring less than min_count times will be discarded from the vocab
		if ((vocab[a].cn < min_count) && (a != 0)) {
			vocab_size--;
			free(vocab[a].word);
		}
		else {
			// Hash will be re-computed, as after the sorting it is not actual
			hash = GetWordHash(vocab[a].word);
			while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
			vocab_hash[hash] = a;
			train_words += vocab[a].cn;
		}
	}
	vocab = (struct vocab_word *)realloc(vocab, (vocab_size + 1) * sizeof(struct vocab_word));
	// Allocate memory for the binary tree construction
	for (a = 0; a < vocab_size; a++) {
		vocab[a].code = (char *)calloc(MAX_CODE_LENGTH, sizeof(char));
		vocab[a].point = (int *)calloc(MAX_CODE_LENGTH, sizeof(int));
	}
}

// Reduces the vocabulary by removing infrequent tokens
void ReduceVocab() {
	int a, b = 0;
	unsigned int hash;
	for (a = 0; a < vocab_size; a++) 
		if (vocab[a].cn > min_reduce) {
			vocab[b].cn = vocab[a].cn;
			vocab[b].word = vocab[a].word;
			b++;
		}
		else free(vocab[a].word);
	vocab_size = b;
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	for (a = 0; a < vocab_size; a++) {
		// Hash will be re-computed, as it is not actual
		hash = GetWordHash(vocab[a].word);
		while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
		vocab_hash[hash] = a;
	}
	fflush(stdout);
	min_reduce++;
}

// Create binary Huffman tree using the word counts
// Frequent words will have short uniqe binary codes
void CreateBinaryTree() {
	long long a, b, i, min1i, min2i, pos1, pos2, point[MAX_CODE_LENGTH];
	char code[MAX_CODE_LENGTH];
	long long *count = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
	long long *binary = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
	long long *parent_node = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
	for (a = 0; a < vocab_size; a++) count[a] = vocab[a].cn;
	for (a = vocab_size; a < vocab_size * 2; a++) count[a] = 1e15;
	pos1 = vocab_size - 1;
	pos2 = vocab_size;
	// Following algorithm constructs the Huffman tree by adding one node at a time
	for (a = 0; a < vocab_size - 1; a++) {
		// First, find two smallest nodes 'min1, min2'
		if (pos1 >= 0) {
			if (count[pos1] < count[pos2]) {
				min1i = pos1;
				pos1--;
			}
			else {
				min1i = pos2;
				pos2++;
			}
		}
		else {
			min1i = pos2;
			pos2++;
		}
		if (pos1 >= 0) {
			if (count[pos1] < count[pos2]) {
				min2i = pos1;
				pos1--;
			}
			else {
				min2i = pos2;
				pos2++;
			}
		}
		else {
			min2i = pos2;
			pos2++;
		}
		count[vocab_size + a] = count[min1i] + count[min2i];
		parent_node[min1i] = vocab_size + a;
		parent_node[min2i] = vocab_size + a;
		binary[min2i] = 1;
	}
	// Now assign binary code to each vocabulary word
	for (a = 0; a < vocab_size; a++) {
		b = a;
		i = 0;
		while (1) {
			code[i] = binary[b];
			point[i] = b;
			i++;
			b = parent_node[b];
			if (b == vocab_size * 2 - 2) break;
		}
		vocab[a].codelen = i;
		vocab[a].point[0] = vocab_size - 2;
		for (b = 0; b < i; b++) {
			vocab[a].code[i - b - 1] = code[b];
			vocab[a].point[i - b] = point[b] - vocab_size;
		}
	}
	free(count);
	free(binary);
	free(parent_node);
}

void LearnVocabFromTrainFile() {
	char word[MAX_STRING];
	FILE *fin;
	long long a, i;
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	fin = fopen(train_file, "rb");
	if (fin == NULL) {
		printf("ERROR: training data file not found!\n");
		exit(1);
	}
	vocab_size = 0;
	AddWordToVocab((char *)"</s>");
	while (1) {
		ReadWord(word, fin);
		if (feof(fin)) break;
		train_words++;
		if ((debug_mode > 1) && (train_words % 100000 == 0)) {
			printf("%lldK%c", train_words / 1000, 13);
			fflush(stdout);
		}
		i = SearchVocab(word);
		if (i == -1) {
			a = AddWordToVocab(word);
			vocab[a].cn = 1;
		}
		else vocab[i].cn++;
		if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
	}
	SortVocab();
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);
	}
	file_size = ftell(fin);
	fclose(fin);
}

void SaveVocab() {
	long long i;
	FILE *fo = fopen(save_vocab_file, "wb");
	for (i = 0; i < vocab_size; i++) fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn);
	fclose(fo);
}

void ReadVocab() {
	long long a, i = 0;
	char c;
	char word[MAX_STRING];
	FILE *fin = fopen(read_vocab_file, "rb");
	if (fin == NULL) {
		printf("Vocabulary file not found\n");
		exit(1);
	}
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	vocab_size = 0;
	while (1) {
		ReadWord(word, fin);
		if (feof(fin)) break;
		a = AddWordToVocab(word);
		fscanf(fin, "%lld%c", &vocab[a].cn, &c);
		// if (vocab[a].cn < 50)
		// 	break;
		i++;
	}
	SortVocab();
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);
	}
	fin = fopen(train_file, "rb");
	if (fin == NULL) {
		printf("ERROR: training data file not found!\n");
		exit(1);
	}
	fseek(fin, 0, SEEK_END);
	file_size = ftell(fin);
	fclose(fin);
	printf("%lld\n", vocab_size);
}

// restore from a given file(state)
void ReadPoint() {
	int a, b;
	FILE *fin = fopen(checkpoint, "r");
	fscanf(fin, "%lld %lld %f\n", &vocab_size, &layer1_size, &alpha);
	char c;
	char waste[MAX_STRING];
	for (a = 0; a < vocab_size; ++a) {
		fscanf(fin, "%s ", waste);
		for (b = 0; b < layer1_size; ++b)
			fscanf(fin, "%f ", &syn0[a * layer1_size + b]);
		fscanf(fin, "%c", &c);
	}
	for (a = 0; a < semantic_num; ++a) {
		for (b = 0; b < layer1_size; ++b)
			fscanf(fin, "%f ", &syn_sem[a * layer1_size + b]);
		fscanf(fin, "%c", &c);
	}
	for (a = 0; a < vocab_size * layer1_size; ++a)
		fscanf(fin, "%f ", &(syn1neg[a]));
	fscanf(fin, "%c", &c);
	fclose(fin);
	printf("checkpoint end\n");
}

void FastReadProjection()
{
	long long a, i, num;
	char word[MAX_STRING];
	char c1 = '\n';
	real *waste = (real *)malloc(semantic_num * sizeof(real));
	
	FILE *fi = fopen(read_proj_fast, "rb");

	if (fi == NULL)
	{
		printf("Semantic file not found\n");
		exit(1);
	}

	a = posix_memalign((void **)&proj, 128, (long long)vocab_size * semantic_num * sizeof(real));
	if (proj == NULL)
	{
		printf("Memory allocation failed\n");
		exit(1);
	}

	in_list = (int *)malloc(vocab_size * sizeof(int));
	memset(in_list, 0, sizeof(int) * vocab_size);
	num = 0;

	while(1)
	{
		// if(c1 != '\n')
		// 	raise(SIGINT);
		ReadWord(word, fi);

		if (feof(fi))
			break;

		++num;
		if (num % 10000 == 0)
		{
			printf("%cHave read %lld0K word projections", 13, num / 10000);
			fflush(stdout);
		}

		i = SearchVocab(word);
		if (i == -1)
		{
			// printf("%s is not in the vocabulary!\n", word);
			// raise(SIGINT);
			fread((void *)waste, sizeof(real), semantic_num, fi);
			fscanf(fi, "%c", &c1);
			continue;
			// exit(1);
		}

		if(in_list[i] != 0)
		{
			printf("%s has occurred before!\n", word);
			exit(1);
		}

		in_list[i] = 1;

		fread((void *)&proj[i * semantic_num], sizeof(real), semantic_num, fi);
		fscanf(fi, "%c", &c1);
	}

	printf("\n");

	for (a = 0; a < vocab_size * semantic_num; ++a)
		proj[a] /= 20;

	free(waste);
	fclose(fi);
}

void ReadProjection()
{
	/* This function reads the semantic projections of words
	** from the file pointed by `read_semantic_proj`.
	*/

	long long a, b, i, num;
	char word[MAX_STRING];
	char c1, c2;
	real sum, waste;
	
	FILE *fi = fopen(read_semantic_proj, "rb");

	if (fi == NULL)
	{
		printf("Semantic file not found\n");
		exit(1);
	}

	a = posix_memalign((void **)&proj, 128, (long long)vocab_size * semantic_num * sizeof(real));
	if (proj == NULL)
	{
		printf("Memory allocation failed\n");
		exit(1);
	}

	in_list = (int *)malloc(vocab_size * sizeof(int));
	memset(in_list, 0, sizeof(int) * vocab_size);
	num = 0;

	while (1)
	{
		ReadWord(word, fi);

		if (feof(fi))
			break;

		++num;
		if (num % 10000 == 0)
		{
			printf("%cHave read %lld0K word projections", 13, num / 10000);
			fflush(stdout);
		}

		i = SearchVocab(word);
		if (i == -1)
		{
			for (a = 0; a < semantic_num; ++a)
				fscanf(fi, "%f", &waste);
			fscanf(fi, "%c%c", &c1, &c2);
			continue;
		}

		if (in_list[i] != 0)
		{
			printf("in list array wrong!\n");
			exit(1);
		}

		in_list[i] = 1;

		real *temp = &proj[i * semantic_num];

		for (a = 0; a < semantic_num; ++a)
		{
			fscanf(fi, "%f", &temp[a]);
		}

		fscanf(fi, "%c%c", &c1, &c2); // the trailing new line
	}

	// min-max normalization
	// real *_min = (real *)malloc(sizeof(real) * semantic_num);
	// real *_max = (real *)malloc(sizeof(real) * semantic_num);

	// for (a = 0; a < semantic_num; ++a)
	// {
	// 	_min[a] = 1e8;
	// 	_max[a] = -1e8;
	// }

	// for (a = 0; a < vocab_size; ++a)
	// {
	// 	if (in_list[a] == 0)
	// 		continue;
	// 	real *temp = &proj[a * semantic_num];
	// 	for (b = 0; b < semantic_num; ++b)
	// 	{
	// 		if (temp[b] < _min[b])
	// 			_min[b] = temp[b];
	// 		if (temp[b] > _max[b])
	// 			_max[b] = temp[b];
	// 	}
	// }

	// for (a = 0; a < vocab_size; ++a)
	// {
	// 	if (in_list[a] == 0)
	// 		continue;

	// 	sum = 0;
	// 	real *temp = &proj[a * semantic_num];

	// 	for (b = 0; b < semantic_num; ++b)
	// 	{
	// 		temp[b] = (temp[b] - _min[b]) / (_max[b] - _min[b]);
	// 		sum += temp[b];
	// 	}

	// 	for (b = 0; b < semantic_num; ++b)
	// 		temp[b] /= sum;
	// }

	// printf("\nNormalization completed\n");

	// free(_min);
	// free(_max);
	fclose(fi);

	fi = fopen("D:\\Users\\v-rumao\\codes\\Sememe\\word_sememe_unnorm.bin", "wb");

	for (a = 0; a < vocab_size; ++a)
	{
		if(in_list[a] == 0)
			continue;
		fprintf(fi, "%s ", vocab[a].word);
		fwrite((void *)&proj[a * semantic_num], sizeof(real), semantic_num, fi);
		fprintf(fi, "\n");
	}
	fclose(fi);
}

// init some data structures
void InitNet() {
	long long a, b;
	a = posix_memalign((void **)&syn0, 128, (long long)vocab_size * layer1_size * sizeof(real));
	if (syn0 == NULL) { printf("Memory allocation failed\n"); exit(1); }

	a = posix_memalign((void **)&syn_sem, 128, (long long)semantic_num * layer1_size * sizeof(real));
	if (syn_sem == NULL)
	{
		printf("Memory allocation failed\n");
		exit(1);
	}
	
	if (negative > 0) {
		a = posix_memalign((void **)&syn1neg, 128, (long long)vocab_size * layer1_size * sizeof(real));
		if (syn1neg == NULL) { printf("Memory allocation failed\n"); exit(1); }
		for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++)
			syn1neg[a * layer1_size + b] = 0;
	}

	for (a = 0; a < vocab_size; ++a)
		for (b = 0; b < layer1_size; ++b)
		{
			next_random = next_random * (unsigned long long)25214903917 + 11;
			syn0[a * layer1_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5) / layer1_size;
		}

	for (a = 0; a < semantic_num; ++a)
		for (b = 0; b < layer1_size; ++b)
		{
			next_random = next_random * (unsigned long long)25214903917 + 11;
			syn_sem[a * layer1_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5) / layer1_size;
		}
	//CreateBinaryTree();
}

void *TrainModelThread(void *id) {
	long long a, b, d, word, last_word, sentence_length = 0, sentence_position = 0;
	long long p;
	long long word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1];
	long long l1, l2, l3, l4, c, target, label, local_iter = iter;
	real f, g;
	clock_t now;
	real *neu1e = (real *)calloc(layer1_size, sizeof(real));
	real *word_vec, *word_proj;
	FILE *fi = fopen(train_file, "rb");
	fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);

	real *input_embed = (real *)calloc(layer1_size, sizeof(real)); // this is for the input word in skip-gram

	while (1) {
		if (word_count - last_word_count > 10000) { // update alpha and some other params
			word_count_actual += word_count - last_word_count;
			last_word_count = word_count;
			if ((debug_mode > 1)) {
				now = clock();
				printf("%cAlpha: %f  Progress: %.2f%%  Words/thread/sec: %.2fk  ", 13, alpha,
					word_count_actual / (real)(iter * train_words + 1) * 100,
					word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
				fflush(stdout);
				if (word_count_actual / (real)(iter * train_words + 1) * 100 > 85) break;
			}
			alpha = starting_alpha * (1 - word_count_actual / (real)(iter * train_words + 1));
			if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;
		}
		if (sentence_length == 0) { // get new sentence to train
			while (1) {
				word = ReadWordIndex(fi);
				if (feof(fi)) {
					break;
				}
				if (word == -1) {
					continue;
				}
				word_count++;
				if (sample > 0) {
					real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
					next_random = next_random * (unsigned long long)25214903917 + 11;
					if (ran < (next_random & 0xFFFF) / (real)65536) continue;
				}
				sen[sentence_length] = word;
				sentence_length++;
				if (sentence_length >= MAX_SENTENCE_LENGTH) break;
			}
			sentence_position = 0;
		}
		if (feof(fi) || (word_count > train_words / num_threads)) { // locate the file pointer
			word_count_actual += word_count - last_word_count;
			local_iter--;
			if (local_iter == 0) break;
			word_count = 0;
			last_word_count = 0;
			sentence_length = 0;
			fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
			continue;
		}
		word = sen[sentence_position];
		if (word == -1) continue;
		next_random = next_random * (unsigned long long)25214903917 + 11;
		b = next_random % window;

		for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
			c = sentence_position - window + a;
			if (c < 0) continue;
			if (c >= sentence_length) continue;
			last_word = sen[c];
			if (last_word == -1) continue;
			l1 = last_word * layer1_size;
			l3 = last_word * semantic_num;
			// word_vec = &syn0[last_word * layer1_size];
			// word_proj = &proj[last_word * semantic_num];

			if (in_list[last_word] > 0)
			{
				for (c = 0; c < layer1_size; ++c)
					syn0[l1 + c] = 0;
				
				for (p = 0; p < semantic_num; ++p)
				{
					l4 = p * layer1_size;
					for (c = 0; c < layer1_size; ++c)
						syn0[l1 + c] += proj[l3 + p] * syn_sem[l4 + c];
				}
				// cblas_sgemv(CblasRowMajor,
				// 			CblasTrans,
				// 			semantic_num,
				// 			layer1_size,
				// 			1,
				// 			syn_sem,
				// 			layer1_size,
				// 			&proj[l3],
				// 			1,
				// 			0,
				// 			&syn0[l1],
				// 			1);
			}

			for (c = 0; c < layer1_size; ++c)
				neu1e[c] = 0;

			// NEGATIVE SAMPLING
			if (negative > 0) for (d = 0; d < negative + 1; d++) {
				if (d == 0) {
					target = word;
					label = 1;
				}
				else {
					next_random = next_random * (unsigned long long)25214903917 + 11;
					target = table[(next_random >> 16) % table_size];
					if (target == 0) target = next_random % (vocab_size - 1) + 1;
					if (target == word) continue;
					label = 0;
				}
				l2 = target * layer1_size;
				f = 0;

				// FP
				for (c = 0; c < layer1_size; ++c)
				{
					f += syn0[l1 + c] * syn1neg[l2 + c];
				}

				if (f > MAX_EXP)
					g = (label - 1) * alpha;
				else if (f < -MAX_EXP)
					g = (label - 0) * alpha;
				else
					g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;

				// accumulate the gradients over negative samples
				for (c = 0; c < layer1_size; ++c)
					neu1e[c] += g * syn1neg[c + l2];

				// BP for the output layer
				for (c = 0; c < layer1_size; ++c)
					syn1neg[c + l2] += g * syn0[l1 + c];
			}

			// BP
			if (in_list[last_word] == 0) // if input word in not in the list, only updates the word embedding
			{
				for (c = 0; c < layer1_size; ++c)
					syn0[l1 + c] += neu1e[c];
			}

			else // update the semantic embeddings
			{
				for (p = 0; p < semantic_num; ++p)
				{
					l4 = p * layer1_size;
					for (c = 0; c < layer1_size; ++c)
					{
						syn_sem[c + l4] += proj[l3 + p] * neu1e[c];
					}
				}
				// cblas_sger(CblasRowMajor,
				// 		   semantic_num,
				// 		   layer1_size,
				// 		   1,
				// 		   &proj[l3],
				// 		   1,
				// 		   neu1e,
				// 		   1,
				// 		   syn_sem,
				// 		   layer1_size);
			}

			// for debugging!
			//if (syn_sem[0] > 10 || syn_sem[0] < -10 || word_count_actual > 0)
			//	raise(SIGINT);
			//for (c = 0; c < layer1_size; c++) syn0[c + l1] += neu1e[c];
		}
		sentence_position++;
		if (sentence_position >= sentence_length) {
			sentence_length = 0;
			continue;
		}
	}

	fclose(fi);
	free(neu1e);
	free(input_embed); // free the preallocated memory

	printf("train end\n");

	pthread_exit(NULL);
}

void TrainModel() {
	long long a, b, c, d;
	FILE *fo;
	pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
	printf("Starting training using file %s\n", train_file);
	starting_alpha = alpha;
	if (read_vocab_file[0] != 0) ReadVocab();
	
	if (read_semantic_proj[0] != 0)
		ReadProjection(); // read the semantic projections
	else if (read_proj_fast[0] != 0)
		FastReadProjection();
	else
	{
		printf("Please specify the semantic file\n");
		exit(1);
	}

	if (save_vocab_file[0] != 0) SaveVocab();
	if (output_file[0] == 0) return;
	InitNet();
	if (negative > 0) InitUnigramTable();
	if (checkpoint[0] != 0) ReadPoint();
	// starting_alpha = alpha;
	start = clock();
	for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
	for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
	// TrainModelThread((void *)0);
	for (a = 0; a < vocab_size; ++a)
	{
		if (in_list[a] == 0)
			continue;
		for (c = 0; c < layer1_size; ++c)
		{
			syn0[a * layer1_size + c] = 0;
			for (b = 0; b < semantic_num; ++b)
			{
				syn0[a * layer1_size + c] += proj[a * semantic_num + b] * syn_sem[b * layer1_size + c];
			}
		}
	}

	fo = fopen(output_file, "w");
	if (classes == 0)
	{
		fprintf(fo, "%lld %lld %f\n", vocab_size, layer1_size, alpha);
		for (a = 0; a < vocab_size; ++a)
		{
			fprintf(fo, "%s ", vocab[a].word);
			for (b = 0; b < layer1_size; ++b)
			{
				fprintf(fo, "%lf ", syn0[a * layer1_size + b]);
			}
			fprintf(fo, "\n");
		}

		for (a = 0; a < semantic_num; ++a)
		{
			for (b = 0; b < layer1_size; ++b)
			{
				fprintf(fo, "%lf ", syn_sem[a * layer1_size + b]);
			}
			fprintf(fo, "\n");
		}

		for (a = 0; a < vocab_size * layer1_size; ++a)
			fprintf(fo, "%lf ", syn1neg[a]);
		fprintf(fo, "\n");
	}
	fclose(fo);
	printf("save end\n");
}

// parse arg
int ArgPos(char *str, int argc, char **argv) {
	int a;
	for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
		if (a == argc - 1) {
			printf("Argument missing for %s\n", str);
			exit(1);
		}
		return a;
	}
	return -1;
}

int main(int argc, char **argv) {
	int i;
	if (argc == 1) {
		printf("WORD VECTOR estimation toolkit v 0.1c\n\n");
		printf("Options:\n");
		printf("Parameters for training:\n");
		printf("\t-train <file>\n");
		printf("\t\tUse text data from <file> to train the model\n");
		printf("\t-output <file>\n");
		printf("\t\tUse <file> to save the resulting word vectors / word clusters\n");
		printf("\t-size <int>\n");
		printf("\t\tSet size of word vectors; default is 100\n");
		printf("\t-window <int>\n");
		printf("\t\tSet max skip length between words; default is 5\n");
		printf("\t-sample <float>\n");
		printf("\t\tSet threshold for occurrence of words. Those that appear with higher frequency in the training data\n");
		printf("\t\twill be randomly down-sampled; default is 1e-3, useful range is (0, 1e-5)\n");
		printf("\t-hs <int>\n");
		printf("\t\tUse Hierarchical Softmax; default is 0 (not used)\n");
		printf("\t-negative <int>\n");
		printf("\t\tNumber of negative examples; default is 5, common values are 3 - 10 (0 = not used)\n");
		printf("\t-threads <int>\n");
		printf("\t\tUse <int> threads (default 12)\n");
		printf("\t-iter <int>\n");
		printf("\t\tRun more training iterations (default 5)\n");
		printf("\t-min-count <int>\n");
		printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
		printf("\t-alpha <float>\n");
		printf("\t\tSet the starting learning rate; default is 0.025 for skip-gram and 0.05 for CBOW\n");
		printf("\t-classes <int>\n");
		printf("\t\tOutput word classes rather than word vectors; default number of classes is 0 (vectors are written)\n");
		printf("\t-debug <int>\n");
		printf("\t\tSet the debug mode (default = 2 = more info during training)\n");
		printf("\t-binary <int>\n");
		printf("\t\tSave the resulting vectors in binary moded; default is 0 (off)\n");
		printf("\t-save-vocab <file>\n");
		printf("\t\tThe vocabulary will be saved to <file>\n");
		printf("\t-read-vocab <file>\n");
		printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
		printf("\t-cbow <int>\n");
		printf("\t\tUse the continuous bag of words model; default is 1 (use 0 for skip-gram model)\n");
		printf("\nExamples:\n");
		printf("./word2vec -train data.txt -output vec.txt -size 200 -window 5 -sample 1e-4 -negative 5 -hs 0 -binary 0 -cbow 1 -iter 3\n\n");
		return 0;
	}
	output_file[0] = 0;
	save_vocab_file[0] = 0;
	read_vocab_file[0] = 0;
	read_meaning_file[0] = 0;
	read_sense_file[0] = 0;
	read_semantic_proj[0] = 0; // initialize the file name to NULL
	read_proj_fast[0] = 0;
	checkpoint[0] = 0;
	if ((i = ArgPos((char *)"-size", argc, argv)) > 0) layer1_size = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-read-meaning", argc, argv)) > 0) strcpy(read_meaning_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-read-sense", argc, argv)) > 0) strcpy(read_sense_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-checkpoint", argc, argv)) > 0) strcpy(checkpoint, argv[i + 1]);
	if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-binary", argc, argv)) > 0) binary = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-cbow", argc, argv)) > 0) cbow = atoi(argv[i + 1]);
	if (cbow) alpha = 0.05;
	if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);
	if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);
	if ((i = ArgPos((char *)"-hs", argc, argv)) > 0) hs = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-iter", argc, argv)) > 0) iter = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-classes", argc, argv)) > 0) classes = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-semantic", argc, argv)) > 0) strcpy(read_semantic_proj, argv[i + 1]); // specify the semantic file
	if ((i = ArgPos((char *)"-semantic-fast", argc, argv)) > 0) strcpy(read_proj_fast, argv[i + 1]); // specify the semantic file (fast version)
	
	vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
	
	vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
	
	expTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
	for (i = 0; i < EXP_TABLE_SIZE; i++) {
		expTable[i] = exp((i / (real)EXP_TABLE_SIZE * 2 - 1) * MAX_EXP); // Precompute the exp() table
		expTable[i] = expTable[i] / (expTable[i] + 1);                   // Precompute f(x) = x / (x + 1)
	}
	for (i = 0; i < EXP_FROM_ZERO_FORE; ++i)
		pre_exp[i] = exp((real)i / (real)(EXP_FROM_ZERO_FORE / 4)); // Precompute the exp() table
	TrainModel();
	return 0;
}
