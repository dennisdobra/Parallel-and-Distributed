# Inverted Index Using Map-Reduce Paradigm

This project implements an **Inverted Index Algorithm** using the **Map-Reduce paradigm**, featuring two types of threads: **Mapper Threads** and **Reducer Threads**.  
The program processes input files and creates an output where words are indexed by the files they appear in, sorted and organized efficiently.

---

## How It Works:

### Mapper Threads

**Mapper threads** are responsible for processing the input files. Here's how they operate:

1. **Input Files Queue**:
   - All files to be processed are stored in a synchronized queue protected by a mutex.
   - Each mapper thread picks one file at a time from the queue.

2. **File Processing**:
   - Once a thread secures a file, it:
     - Reads the file line by line.
     - Splits lines into tokens (words).
     - Processes each token (cleaning non-alphabetical characters and validating it).
     - Adds valid tokens to a partial list specific to the file, avoiding duplicates using a set.
     - Closes the file after the processing stage is finished.

3. **Synchronization**:
   - Once all files are processed, mapper threads wait at a barrier to ensure their tasks are completed before reducers start.

---

### Reducer Threads

**Reducer threads** handle data aggregation and organization. Here's how:

1. **Data Division**:
   - Reducers divide the partial lists created by mapper threads among themselves.
   - Each reducer combines the partial lists it is responsible for into a local aggregated list, which stores the IDs for all the files in which each word appears.

2. **Shared Resource**:
   - After each reducer thread finishes creating its own aggregated list, all the lists are added to a shared resource (`finalAggregatedList`).
   - A barrier ensures this step is complete before further processing.
   - Then, this `finalAggregatedList` has its file IDs for each word sorted in ascending order by one of the reducers.
   - A barrier ensures this step is complete before further processing.

3. **Letter Processing**:
   - Each reducer is responsible for a set of letters from the alphabet.
   - For each letter a reducer is responsible for, it creates a specific file.
   - Then, it iterates through the `finalAggregatedList` and groups all the words starting with the current letter into a local vector.
   - This vector is then sorted accordingly, and the words are written in the corresponding file.
