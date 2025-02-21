#include <mpi.h>
#include <pthread.h>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <cstdlib> // Pentru rand()
#include <ctime>   // Pentru srand()
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <iostream>
using namespace std;

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define INIT_MSG_FROM_PEER 0
#define FILE_REQUEST_MSG 1
#define RESEND_SEEDERS_MSG 2
#define REQUEST_SEGMENT 3
#define RECEIVED_ALL_FILES 4
#define CLIENT_CLOSE_UPLOAD 5

map<string, vector<string>> owned_files_by_peer;
map<string, vector<string>> owned_files_by_tracker;
map<string, set<int>> seeders_map;
vector<string> wanted_files;

void send_init_msg_w_owned_files(int client_rank) {
    /* Announce tracker that i am going to send Init message */
    int msg_type = INIT_MSG_FROM_PEER;
    MPI_Send(&msg_type, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    /* Send number of owned files */
    int num_files = owned_files_by_peer.size();
    MPI_Send(&num_files, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    for (const auto &[file_name, segments] : owned_files_by_peer) {
        /* Send filename */
        MPI_Send(file_name.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

        /* Send all the segments for the file */
        int num_segments = segments.size();
        MPI_Send(&num_segments, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        for (const auto& segment : segments) {
            MPI_Send(segment.c_str(), HASH_SIZE, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        }
    }
}

void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;

    for (const auto& file : wanted_files) {
        /* Send file request signal to tracker */
        int msg_type = FILE_REQUEST_MSG;
        MPI_Send(&msg_type, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        /* Send the name of the file to tracker and wait for seeders list + corresponding hashes */
        MPI_Send(file.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);

        /* Receive the hashes */
        int num_hashes;
        MPI_Recv(&num_hashes, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        vector<string> hashes(num_hashes);
        for (int i = 0; i < num_hashes; i++) {
            char recv_hash[HASH_SIZE];
            MPI_Recv(recv_hash, HASH_SIZE, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            hashes[i] = recv_hash;
        }

        /* Receive the seeders */
        int num_seeds;
        MPI_Recv(&num_seeds, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        vector<int> seeds(num_seeds);
        for (int i = 0; i < num_seeds; i++) {
            MPI_Recv(&seeds[i], 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
        
        /* Now that the client has the neccessary information it can start requesting segments */

        int recv_segments = 0;
        int num_request_seeds = 0;
        int seeder_index = 0;
        while (recv_segments < num_hashes) {
            /* Request one segment at a time, up to 10 */
            for (int step = 0; step < 10 && recv_segments < num_hashes; step++) {
                bool segment_ok = false;
                int segment_idx = recv_segments;

                while (!segment_ok) {
                    int chosen_seeder = seeds[seeder_index];
                    seeder_index = (seeder_index + 1) % num_seeds;

                    /* check if I am not the chosen seeder */
                    if (chosen_seeder == rank) {
                        continue;
                    }

                    /* Request segment */
                    int msg_type = REQUEST_SEGMENT;
                    MPI_Send(&msg_type, 1, MPI_INT, chosen_seeder, 2, MPI_COMM_WORLD);
                    MPI_Send(file.c_str(), MAX_FILENAME, MPI_CHAR, chosen_seeder, 2, MPI_COMM_WORLD);
                    MPI_Send(&segment_idx, 1, MPI_INT, chosen_seeder, 2, MPI_COMM_WORLD);

                    /* Wait for response from chosen seeder */
                    char recv_segment[HASH_SIZE];
                    int response_tag = 100 + segment_idx;
                    MPI_Recv(recv_segment, HASH_SIZE, MPI_CHAR, chosen_seeder, response_tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    /* Check if I received what I was expecting */
                    if (strcmp(recv_segment, hashes[segment_idx].c_str()) == 0) {
                        recv_segments++;
                        segment_ok = true;
                        owned_files_by_peer[file].push_back(recv_segment);
                    }
                }
            }
            
            /* Request actualized list of seeder for this file */
            if (recv_segments < num_hashes) {
                num_request_seeds++;
                int update_msg_type = RESEND_SEEDERS_MSG;
                MPI_Send(&update_msg_type, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

                /* Send filename */
                MPI_Send(file.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);

                /* Receive actualized list of seeders */
                MPI_Recv(&num_seeds, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                seeds.resize(num_seeds);
                for (int i = 0; i < num_seeds; i++) {
                    MPI_Recv(&seeds[i], 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                }
            }
        }

        /* Now that I have received the entire file, I have to write all the hashes in an output file */
        string output_filename = "client" + std::to_string(rank) + "_" + file;

        ofstream outfile(output_filename);
        if (!outfile.is_open()) {
            cout << "Client " << rank << " failed to open file for writing: " << output_filename << std::endl;
            break;
        }

        for (size_t i = 0; i < owned_files_by_peer[file].size(); ++i) {
            outfile << owned_files_by_peer[file][i];
            if (i < owned_files_by_peer[file].size() - 1) {
                outfile << "\n";
            }
        }

        outfile.close();
    }

    /* Announce tracker that all wanted files were received */
    int msg_type = RECEIVED_ALL_FILES;
    MPI_Send(&msg_type, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    int rank = *(int*) arg;

    while (true) {
        MPI_Status status;
        int msg_type;

        MPI_Recv(&msg_type, 1, MPI_INT, MPI_ANY_SOURCE, 2, MPI_COMM_WORLD, &status);

        if (msg_type == REQUEST_SEGMENT) {
            int requesting_peer = status.MPI_SOURCE;

            /* Receive the requested file and the index of the requested segment */
            char requested_file[MAX_FILENAME];
            int segment_idx;

            MPI_Recv(requested_file, MAX_FILENAME, MPI_CHAR, requesting_peer, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Recv(&segment_idx, 1, MPI_INT, requesting_peer, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            /* Check if I have the requested segment */
            const auto &segments = owned_files_by_peer[requested_file];
            if (segment_idx < (int)segments.size()) {
                const auto &req_segment = segments[segment_idx];
                int response_tag = 100 + segment_idx;
                MPI_Send(req_segment.c_str(), HASH_SIZE, MPI_CHAR, requesting_peer, response_tag, MPI_COMM_WORLD);
            } else {
                const char *nack_msg = "NACK";
                int response_tag = 100 + segment_idx;
                MPI_Send(nack_msg, HASH_SIZE, MPI_CHAR, requesting_peer, response_tag, MPI_COMM_WORLD);
            }
        } else if (msg_type == CLIENT_CLOSE_UPLOAD) {
            break;
        }
    }

    return NULL;
}

void tracker(int numtasks, int rank) {
    int clients_got_wanted_files = 0;
    int init_messages_received = 0;

    while (clients_got_wanted_files != numtasks - 1) {
        /* All clients finished initialization process */
        if (init_messages_received == numtasks - 1) {
            for (int client_rank = 1; client_rank < numtasks; client_rank++) {
                const char *ack_msg = "ACK";
                MPI_Send(ack_msg, strlen(ack_msg) + 1, MPI_CHAR, client_rank, 0, MPI_COMM_WORLD);
            }
            init_messages_received = -1;
        }

        /* All clients received all wanted files */
        if (clients_got_wanted_files == numtasks - 1) {
            break;
        }

        MPI_Status status;
        int request_msg;
        MPI_Recv(&request_msg, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

        /* Tracker responds according to the request message */
        if (request_msg == INIT_MSG_FROM_PEER) {
            init_messages_received++;
            int client_rank = status.MPI_SOURCE;

            /* Receive number of files from client */
            int num_files;
            MPI_Recv(&num_files, 1, MPI_INT, client_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            /* Receive all files one by one */
            for (int i = 0; i < num_files; i++) {
                char file_name[MAX_FILENAME];
                MPI_Recv(file_name, MAX_FILENAME, MPI_CHAR, client_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                /* Receive nr of segments + segments */
                int num_segments;
                MPI_Recv(&num_segments, 1, MPI_INT, client_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                vector<string> segments;
                for (int j = 0; j < num_segments; j++) {
                    char curr_segment[HASH_SIZE];
                    MPI_Recv(curr_segment, HASH_SIZE, MPI_CHAR, client_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    
                    segments.emplace_back(curr_segment);
                }

                owned_files_by_tracker[file_name] = segments;
                seeders_map[file_name].insert(client_rank);
            }
        } else if (request_msg == FILE_REQUEST_MSG) {
            /* Get the source of the message */
            int requesting_peer = status.MPI_SOURCE;

            /* Receive requested file */
            char requested_file[MAX_FILENAME];
            MPI_Recv(requested_file, MAX_FILENAME, MPI_CHAR, requesting_peer, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            /* Mark the client as seed for the file */
            seeders_map[requested_file].insert(requesting_peer);

            /* Send nr of hashes + list of hashes */
            const auto &hashes = owned_files_by_tracker[requested_file];
            int num_hashes = hashes.size();
            MPI_Send(&num_hashes, 1, MPI_INT, requesting_peer, 1, MPI_COMM_WORLD);
            for (const auto &hash : hashes) {
                MPI_Send(hash.c_str(), HASH_SIZE, MPI_CHAR, requesting_peer, 1, MPI_COMM_WORLD);
            }

            /* Send nr of seeders + list of seeders */
            const auto &seeders = seeders_map[requested_file];
            int num_seeders = seeders.size();
            MPI_Send(&num_seeders, 1, MPI_INT, requesting_peer, 1, MPI_COMM_WORLD);
            for (const auto &seeder : seeders) {
                MPI_Send(&seeder, 1, MPI_INT, requesting_peer, 1, MPI_COMM_WORLD);
            }
        } else if (request_msg == RESEND_SEEDERS_MSG) {
            char requested_file[MAX_FILENAME];
            int requesting_peer = status.MPI_SOURCE;

            MPI_Recv(requested_file, MAX_FILENAME, MPI_CHAR, requesting_peer, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            /* Resend nr of seeders + list of seeders */
            const auto &seeders = seeders_map[requested_file];
            int num_seeders = seeders.size();
            MPI_Send(&num_seeders, 1, MPI_INT, requesting_peer, 1, MPI_COMM_WORLD);
            for (const auto &seeder : seeders) {
                MPI_Send(&seeder, 1, MPI_INT, requesting_peer, 1, MPI_COMM_WORLD);
            }
        } else if (request_msg == RECEIVED_ALL_FILES) {
            clients_got_wanted_files++;
        }
    }

    /* Send all clients a message to close upload loop */
    for (int client_rank = 1; client_rank < numtasks; client_rank++) {
        int msg_type = CLIENT_CLOSE_UPLOAD;
        MPI_Send(&msg_type, 1, MPI_INT, client_rank, 2, MPI_COMM_WORLD);
    }
}

void parse_input_file(FILE* file) {
    /* Parse owned files */
    int nr_owned_files;
    fscanf(file, "%d", &nr_owned_files);

    for (int i = 0; i < nr_owned_files; i++) {
        char file_name[MAX_FILENAME];
        int num_segments;

        fscanf(file, "%s %d", file_name, &num_segments);

        vector<string> segments;
        for (int j = 0; j < num_segments; j++) {
            char curr_segment[HASH_SIZE];
            fscanf(file, "%s", curr_segment);
            owned_files_by_peer[file_name].push_back(curr_segment);
        }
    }

    /* Parse wanted files */
    int nr_wanted_files;
    fscanf(file, "%d", &nr_wanted_files);

    for (int i = 0; i < nr_wanted_files; i++) {
        char wanted_file[MAX_FILENAME];

        fscanf(file, "%s", wanted_file);
        wanted_files.emplace_back(wanted_file);
    }
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    /* Get the input file */
    char inputFile[MAX_FILENAME];
    snprintf(inputFile, sizeof(inputFile), "in%d.txt", rank);

    FILE *file = fopen(inputFile, "r");
    if (file == NULL) {
        printf("Error opening input file: %s\n", inputFile);
        exit(-1);
    }

    /* Parse the input file */
    parse_input_file(file);
    fclose(file);

    /* Send owned files to tracker */
    send_init_msg_w_owned_files(rank);

    /* Wait for ACK from tracker to start communication */
    char ack_msg[10];
    MPI_Recv(ack_msg, sizeof(ack_msg), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) { 
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}