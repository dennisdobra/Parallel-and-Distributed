
#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <queue>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>

using namespace std;

struct ThreadArgs {
    long id;
    int NrMapperThreads;
    int NrReducerThreads;

    queue<pair<string, int>>* filesQueue;                // queue to store all the files that need to be processed
    vector<vector<pair<string, int>>>* allPartialLists;  // vector to store all the vectors of words from each files
    map<string, vector<int>>* finalAggreagtedList;       // map to store each unique word from all the files + all the files the word appears in

    pthread_mutex_t* queueMutex;
    pthread_mutex_t* printMutex;
    pthread_mutex_t* finalAggreagtedListMutex;
    
    pthread_barrier_t* barrier;                     // barrier waiting for NrMapperThreads + NrReducerThreads
    pthread_barrier_t* reducerBarrier;              // barrier waiting only for Reducer threads
};

string modifyString(string s) {
    for (auto& c : s) {
        if (isupper(c)) c = tolower(c);
    }

    // remove non alphabetical characters
    s.erase(remove_if(s.begin(), s.end(), [](char c) { return !isalpha(c); }), s.end());

    return s;
}

void* threadFunc(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;

    if (args->id <= args->NrMapperThreads) {
        while (true) {
            pair<string, int> fileToProcess;

            /* safely fetch a file from the queue */
            pthread_mutex_lock(args->queueMutex);
            if (!args->filesQueue->empty()) {
                fileToProcess = args->filesQueue->front();
                args->filesQueue->pop();
            }
            pthread_mutex_unlock(args->queueMutex);

            if (fileToProcess.first.empty()) break;

            /* Process the file */
            string fullPath = "../checker/" + fileToProcess.first;
            FILE* file = fopen(fullPath.c_str(), "r");
            if (!file) {
                pthread_mutex_lock(args->printMutex);
                cout << "Couldn't open file: " << fileToProcess.first << endl;
                pthread_mutex_unlock(args->printMutex);
                continue;
            }

            // partial list for the words in the current file
            vector<pair<string, int>>& partialList = (*args->allPartialLists)[fileToProcess.second - 1];
            // local set to avoid having duplicates in partialList
            set<string> uniqueWords; 

            char line[3000]; // 3000 because lines are very long in the files
            while (fgets(line, sizeof(line), file)) {
                string stringLine(line); // convert for safety

                size_t pos = 0;
                while ((pos = stringLine.find_first_of(" \n")) != string::npos) {
                    string token = stringLine.substr(0, pos); // get the string from the beginning to the first withespace or '\n'
                    stringLine.erase(0, pos + 1);             // remove the token from the line
                    
                    string modifiedWord = modifyString(token);
                    // check if the word is still valid after modification and does not
                    // already have an entry in the set
                    if (!modifiedWord.empty() && uniqueWords.insert(modifiedWord).second) {
                        partialList.emplace_back(modifiedWord, fileToProcess.second);
                    }
                }

                if (!stringLine.empty()) {
                    string modifiedWord = modifyString(stringLine);
                    // same as above
                    if (!modifiedWord.empty() && uniqueWords.insert(modifiedWord).second) {
                        partialList.emplace_back(modifiedWord, fileToProcess.second);
                    }
                }
            }

            fclose(file);
        }

        pthread_barrier_wait(args->barrier);

    } else {
        pthread_barrier_wait(args->barrier);

        size_t chunk, start, end;

        // each reducer will merge a number of partial lists in a local aggregated list
        size_t allPartialListsize = args->allPartialLists->size();
        chunk = (allPartialListsize + args->NrReducerThreads - 1) / args->NrReducerThreads;
        start = (args->id - args->NrMapperThreads - 1) * chunk;
        end = min(allPartialListsize, start + chunk);

        // merge the lists he is responsible for
        unordered_map<string, unordered_set<int>> aggreagtedList;
        for (size_t i = start; i < end; ++i) {
            for (const auto& wordPair : (*args->allPartialLists)[i]) {
                aggreagtedList[wordPair.first].insert(wordPair.second);
            }
        }

        // ensure the final merge (of all aggregated lists) does not start before each thread has completed its own aggregated list
        pthread_barrier_wait(args->reducerBarrier);


        // merge all aggregated lists in shared finalAggregatedList
        pthread_mutex_lock(args->finalAggreagtedListMutex);
        for (auto& [word, fileIDs] : aggreagtedList) {
            auto& globalFileIDs = (*args->finalAggreagtedList)[word];
            globalFileIDs.insert(globalFileIDs.end(), fileIDs.begin(), fileIDs.end());
        }
        pthread_mutex_unlock(args->finalAggreagtedListMutex);


        // wait for all reducers to add their aggregated list
        pthread_barrier_wait(args->reducerBarrier);

        // Now, sort and remove duplicates for all accumulated file IDs for each word in the finalAggregatedList
        if (args->id == args->NrMapperThreads + 1) {
            for (auto& [word, fileIDs] : *args->finalAggreagtedList) {
                // Sort the file IDs and remove duplicates
                sort(fileIDs.begin(), fileIDs.end());
                fileIDs.erase(unique(fileIDs.begin(), fileIDs.end()), fileIDs.end());
            }
        }

        // wait for the list to be sorted
        pthread_barrier_wait(args->reducerBarrier);

        // each reducer thread is responsible for a set of letters
        size_t totalLetters = 26;
        chunk = (totalLetters + args->NrReducerThreads - 1) / args->NrReducerThreads;
        start = (args->id - args->NrMapperThreads - 1) * chunk;
        end = min(totalLetters, start + chunk);

        for (size_t i = start; i < end; i++) {
            char letter = 'a' + i;
            string filename = string(1, letter) + ".txt";

            ofstream outFile(filename);
            if (!outFile) {
                pthread_mutex_lock(args->printMutex);
                cout << "Error creating file: " << filename << endl;
                pthread_mutex_unlock(args->printMutex);
                continue;
            }

            // iterate through the finalAggregatedList and group
            // the words starting with the current letter
            vector<pair<string, vector<int>>> wordsWithCurrentLetter;
            for (const auto& [word, fileIDs] : *args->finalAggreagtedList) {
                if (!word.empty() && word[0] == letter) {
                    wordsWithCurrentLetter.emplace_back(word, fileIDs);
                }
            }

            // Sort the words by the number of files they appear in descending order or alphabetically for ties
            sort(wordsWithCurrentLetter.begin(), wordsWithCurrentLetter.end(),
                 [](const auto& a, const auto& b) {
                     if (a.second.size() != b.second.size()) {
                         return a.second.size() > b.second.size();
                     }
                     return a.first < b.first;
                 });

            // write the sorted vector in the file
            for (const auto& [word, IDs] : wordsWithCurrentLetter) {
                outFile << word << ":["; 
                for (size_t j = 0; j < IDs.size(); j++) {
                    outFile << IDs[j];
                    if (j != IDs.size() - 1) {
                        outFile << " ";
                    }
                }
                outFile << "]" << endl;
            }

            outFile.close();
        }
    }

    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        cout << "Usage: " << argv[0] << " <nr_mapper_threads> <nr_reducer_threads> <input_file>" << endl;
        return 1;
    }

    int NrMapperThreads = atoi(argv[1]);
    int NrReducerThreads = atoi(argv[2]);
    const char* inputFile = argv[3];

    FILE* file = fopen(inputFile, "r");
    if (!file) {
        cout << "Could not open input file: " << inputFile << endl;
        return 1;
    }

    int NrOfFiles;
    if (fscanf(file, "%d", &NrOfFiles) != 1 || NrOfFiles <= 0) {
        cout << "Invalid format for input file" << endl;
        fclose(file);
        return 1;
    }

    // Create the queue with files to process
    queue<pair<string, int>> filesQueue;
    for (int i = 0; i < NrOfFiles; i++) {
        char fileName[256];
        if (fscanf(file, "%s", fileName) != 1) {
            cout << "Error reading file name" << endl;
            fclose(file);
            return 1;
        }
        // emplace is more efficient than push
        filesQueue.emplace(fileName, i + 1);
    }
    fclose(file);


    // lists for mapper threads -> vector storing a list of words for each file
    vector<vector<pair<string, int>>> allPartialLists(NrOfFiles);

    // map with all the unique words from all files
    map<string, vector<int>> allUniqueWords;

    int TotalThreads = NrMapperThreads + NrReducerThreads;
    pthread_t threads[TotalThreads];
    ThreadArgs arguments[TotalThreads];

    // synchronization primitives
    pthread_barrier_t barrier;
    pthread_barrier_t reducerBarrier;

    pthread_mutex_t queueMutex;
    pthread_mutex_t printMutex;
    pthread_mutex_t mapMutex; // mutex for finalAggregatedList map

    pthread_barrier_init(&barrier, NULL, TotalThreads);
    pthread_barrier_init(&reducerBarrier, NULL, NrReducerThreads);
    pthread_mutex_init(&queueMutex, NULL);
    pthread_mutex_init(&printMutex, NULL);
    pthread_mutex_init(&mapMutex, NULL);

    for (int id = 0; id < TotalThreads; id++) {
        arguments[id] = {id + 1, NrMapperThreads, NrReducerThreads, &filesQueue, &allPartialLists, &allUniqueWords,
                         &queueMutex, &printMutex, &mapMutex, &barrier, &reducerBarrier};

        pthread_create(&threads[id], NULL, threadFunc, &arguments[id]);
    }

    for (int id = 0; id < TotalThreads; id++) {
        pthread_join(threads[id], NULL);
    }

    pthread_barrier_destroy(&barrier);
    pthread_barrier_destroy(&reducerBarrier);
    pthread_mutex_destroy(&queueMutex);
    pthread_mutex_destroy(&printMutex);
    pthread_mutex_destroy(&mapMutex);

    return 0;
}
