#ifndef QUICKSORT_PARALLEL_H
#define QUICKSORT_PARALLEL_H

#include "../../Utils/data_types_common.h"
#include "../../Utils/sort_interface.h"
#include "../constants.h"
#include "../data_types.h"

#define __CUDA_INTERNAL_COMPILATION__
#include "../Kernels/common.h"
#include "../Kernels/key_only.h"
#include "../Kernels/key_value.h"
#undef __CUDA_INTERNAL_COMPILATION__


/*
Base class for parallel bitonic sort.
Needed for template specialization.

Template params:
_Ko - Key-only
_Kv - Key-value

TODO implement DESC ordering.
*/
template <
    uint_t thresholdParallelReduction,
    uint_t threadsReduction, uint_t elemsReduction,
    uint_t threasholdPartitionGlobalKo, uint_t threasholdPartitionGlobalKv,
    uint_t threadsSortGlobalKo, uint_t elemsSortGlobalKo,
    uint_t threadsSortGlobalKv, uint_t elemsSortGlobalKv,
    uint_t thresholdBitonicSortKo, uint_t thresholdBitonicSortKv,
    uint_t threadsSortLocalKo, uint_t threadsSortLocalKv
>
class QuicksortParallelBase : public SortParallel
{
protected:
    std::string _sortName = "Quicksort parallel";
    // Device buffer for keys and values
    data_t *_d_keysBuffer, *_d_valuesBuffer;
    // When pivots are scattered in global and local quicksort, they have to be considered as unique elements
    // because of array of values (alongside keys). Because array can contain duplicate keys, values have to
    // be stored in buffer, because end position of pivots isn't known until last thread block processes sequence.
    data_t *_d_valuesPivot;
    // When initial min/max parallel reduction reduces data to threshold, min/max values are copied to host
    // and reduction is finnished on host. Multiplier "2" is used because of min and max values.
    data_t *_h_minMaxValues;
    // Sequences metadata for GLOBAL quicksort on HOST
    h_glob_seq_t *_h_globalSeqHost, *_h_globalSeqHostBuffer;
    // Sequences metadata for GLOBAL quicksort on DEVICE
    d_glob_seq_t *_h_globalSeqDev, *_d_globalSeqDev;
    // Array of sequence indexes for thread blocks in GLOBAL quicksort. This way thread blocks know which
    // sequence they have to partition.
    uint_t *_h_globalSeqIndexes, *_d_globalSeqIndexes;
    // Sequences metadata for LOCAL quicksort
    loc_seq_t *_h_localSeq, *_d_localSeq;
    // Boolean which marks, if the input distribution was null
    bool _isDistributionZero;

    void memoryAllocate(data_t *h_keys, data_t *h_values, uint_t arrayLength)
    {
        SortParallel::memoryAllocate(h_keys, h_values, arrayLength);

        // Min/Max calculations needed, because memory is allocated both for key only and for key-value sort
        uint_t minPartitionSizeGlobal = min(threasholdPartitionGlobalKo, threasholdPartitionGlobalKv);
        uint_t maxPartitionSizeGlobal = max(threasholdPartitionGlobalKo, threasholdPartitionGlobalKv);
        uint_t minElemsPerThreadBlock = min(
            threadsSortGlobalKo * elemsSortGlobalKo, threadsSortGlobalKv * elemsSortGlobalKv
        );

        // Maximum number of sequences which can get generated by global quicksort. In global quicksort sequences
        // are generated until total number of sequences is lower than: tableLen / THRESHOLD_PARTITION_SIZE_GLOBAL.
        uint_t maxNumSequences = 2 * ((arrayLength - 1) / minPartitionSizeGlobal + 1);
        // Max number of all thread blocks in GLOBAL quicksort.
        uint_t maxNumThreadBlocks = maxNumSequences * ((maxPartitionSizeGlobal - 1) / minElemsPerThreadBlock + 1);
        cudaError_t error;

        /* HOST MEMORY */

        // Sequence metadata memory allocation
        _h_globalSeqHost = (h_glob_seq_t*)malloc(maxNumSequences * sizeof(*_h_globalSeqHost));
        checkMallocError(_h_globalSeqHost);
        _h_globalSeqHostBuffer = (h_glob_seq_t*)malloc(maxNumSequences * sizeof(*_h_globalSeqHostBuffer));
        checkMallocError(_h_globalSeqHostBuffer);

        // These sequences are transferred between host and device and are therefore allocated in CUDA pinned memory
        error = cudaHostAlloc(
            (void **)&_h_minMaxValues, 2 * thresholdParallelReduction * sizeof(*_h_minMaxValues),
            cudaHostAllocDefault
        );
        checkCudaError(error);
        error = cudaHostAlloc(
            (void **)&_h_globalSeqDev, maxNumSequences * sizeof(*_h_globalSeqDev), cudaHostAllocDefault
        );
        checkCudaError(error);
        error = cudaHostAlloc(
            (void **)&_h_globalSeqIndexes, maxNumThreadBlocks * sizeof(*_h_globalSeqIndexes), cudaHostAllocDefault
        );
        checkCudaError(error);
        error = cudaHostAlloc(
            (void **)&_h_localSeq, maxNumSequences * sizeof(*_h_localSeq), cudaHostAllocDefault
        );
        checkCudaError(error);

        /* DEVICE_MEMORY */

        error = cudaMalloc((void **)&_d_keysBuffer, arrayLength * sizeof(*_d_keysBuffer));
        checkCudaError(error);
        error = cudaMalloc((void **)&_d_valuesBuffer, arrayLength * sizeof(*_d_valuesBuffer));
        checkCudaError(error);
        error = cudaMalloc((void **)&_d_valuesPivot, arrayLength * sizeof(*_d_valuesPivot));
        checkCudaError(error);

        // Sequence metadata memory allocation
        error = cudaMalloc((void **)&_d_globalSeqDev, maxNumSequences * sizeof(*_d_globalSeqDev));
        checkCudaError(error);
        error = cudaMalloc((void **)&_d_globalSeqIndexes, maxNumThreadBlocks * sizeof(*_d_globalSeqIndexes));
        checkCudaError(error);
        error = cudaMalloc((void **)&_d_localSeq, maxNumSequences * sizeof(*_d_localSeq));
        checkCudaError(error);
    }

    /*
    If input distribution is zero, then the sorted array is contained in primary arrays, elese in buffer arrays.
    */
    void memoryCopyAfterSort(data_t *h_keys, data_t *h_values, uint_t arrayLength)
    {
        cudaError_t error;

        if (_isDistributionZero)
        {
            SortParallel::memoryCopyAfterSort(h_keys, h_values, arrayLength);
        }
        else
        {
            // Copies keys
            error = cudaMemcpy(
                h_keys, (void *)_d_keysBuffer, _arrayLength * sizeof(*_h_keys), cudaMemcpyDeviceToHost
            );
            checkCudaError(error);

            if (h_values == NULL)
            {
                return;
            }

            // Copies values
            error = cudaMemcpy(
                h_values, (void *)_d_valuesBuffer, arrayLength * sizeof(*h_values), cudaMemcpyDeviceToHost
            );
            checkCudaError(error);
        }
    }

    /*
    Executes kernel for finding min/max values. Every thread block searches for min/max values in their
    corresponding chunk of data. This means kernel will return a list of min/max values with same length
    as number of thread blocks executing in kernel.
    */
    uint_t runMinMaxReductionKernel(data_t *d_keys, data_t *d_keysBuffer, uint_t arrayLength)
    {
        // Half of the array for min values and the other half for max values
        uint_t sharedMemSize = 2 * threadsReduction * sizeof(*d_keys);
        dim3 dimGrid((arrayLength - 1) / (threadsReduction * elemsReduction) + 1, 1, 1);
        dim3 dimBlock(threadsReduction, 1, 1);

        minMaxReductionKernel<threadsReduction, elemsReduction><<<dimGrid, dimBlock, sharedMemSize>>>(
            d_keys, d_keysBuffer, arrayLength
        );

        return dimGrid.x;
    }

    /*
    Searches for min/max values in array.
    */
    void minMaxReduction(
        data_t *h_keys, data_t *d_keys, data_t *d_keysBuffer, data_t *h_minMaxValues, uint_t arrayLength,
        data_t &minVal, data_t &maxVal
    )
    {
        minVal = MAX_VAL;
        maxVal = MIN_VAL;

        // Checks whether array is short enough to be reduced entirely on host or if reduction on device is needed
        if (arrayLength > thresholdParallelReduction)
        {
            // Kernel returns array with min/max values of length numVales
            uint_t numValues = runMinMaxReductionKernel(d_keys, d_keysBuffer, arrayLength);

            cudaError_t error = cudaMemcpy(
                h_minMaxValues, d_keysBuffer, 2 * numValues * sizeof(*h_minMaxValues), cudaMemcpyDeviceToHost
            );
            checkCudaError(error);

            data_t *minValues = h_minMaxValues;
            data_t *maxValues = h_minMaxValues + numValues;

            // Finishes reduction on host
            for (uint_t i = 0; i < numValues; i++)
            {
                minVal = min(minVal, minValues[i]);
                maxVal = max(maxVal, maxValues[i]);
            }
        }
        else
        {
            for (uint_t i = 0; i < arrayLength; i++)
            {
                minVal = min(minVal, h_keys[i]);
                maxVal = max(maxVal, h_keys[i]);
            }
        }
    }

    /*
    Runs global (multiple thread blocks process one sequence) quicksort and copies required data to and
    from device.
    */
    template <order_t sortOrder, bool sortingKeyOnly>
    void runQuickSortGlobalKernel(
        data_t *d_keys, data_t *d_values, data_t *d_keysBuffer, data_t *d_valuesBuffer, data_t *d_valuesPivot,
        d_glob_seq_t *h_globalSeqDev, d_glob_seq_t *d_globalSeqDev, uint_t *h_globalSeqIndexes,
        uint_t *d_globalSeqIndexes, uint_t numSeqGlobal, uint_t threadBlockCounter
    )
    {
        cudaError_t error;
        uint_t threadsSortGlobal = sortingKeyOnly ? threadsSortGlobalKo : threadsSortGlobalKv;

        // 1. arg: Size of array for calculation of min/max value ("2" because of MIN and MAX)
        // 2. arg: Size of array needed to perform scan of counters for number of elements lower/greater than
        //         pivot ("2" because of intra-warp scan).
        uint_t sharedMemSize = 2 * threadsSortGlobal * max(sizeof(data_t), sizeof(uint_t));
        dim3 dimGrid(threadBlockCounter, 1, 1);
        dim3 dimBlock(threadsSortGlobal, 1, 1);

        error = cudaMemcpy(
            d_globalSeqDev, h_globalSeqDev, numSeqGlobal * sizeof(*d_globalSeqDev), cudaMemcpyHostToDevice
        );
        checkCudaError(error);
        error = cudaMemcpy(
            d_globalSeqIndexes, h_globalSeqIndexes, threadBlockCounter * sizeof(*d_globalSeqIndexes),
            cudaMemcpyHostToDevice
        );
        checkCudaError(error);

        if (sortingKeyOnly)
        {
            quickSortGlobalKernel
                <threadsSortGlobalKo, elemsSortGlobalKo, sortOrder><<<dimGrid, dimBlock, sharedMemSize>>>(
                d_keys, d_keysBuffer, d_globalSeqDev, d_globalSeqIndexes
            );
        }
        else
        {
            quickSortGlobalKernel
                <threadsSortGlobalKv, elemsSortGlobalKv, sortOrder><<<dimGrid, dimBlock, sharedMemSize>>>(
                d_keys, d_values, d_keysBuffer, d_valuesBuffer, d_valuesPivot, d_globalSeqDev, d_globalSeqIndexes
            );
        }

        error = cudaMemcpy(
            h_globalSeqDev, d_globalSeqDev, numSeqGlobal * sizeof(*h_globalSeqDev), cudaMemcpyDeviceToHost
        );
        checkCudaError(error);
    }

    /*
    Finishes quicksort with local (one thread block processes one block) quicksort.
    */
    template <order_t sortOrder, bool sortingKeyOnly>
    void runQuickSortLocalKernel(
        data_t *d_keys, data_t *d_values, data_t *d_keysBuffer, data_t *d_valuesBuffer, data_t *d_valuesPivot,
        loc_seq_t *h_localSeq, loc_seq_t *d_localSeq, uint_t numThreadBlocks
    )
    {
        cudaError_t error;
        uint_t threadsSortLocal = sortingKeyOnly ? threadsSortLocalKo : threadsSortLocalKv;
        uint_t thresholdBitonicSort = sortingKeyOnly ? thresholdBitonicSortKo : thresholdBitonicSortKv;

        // The same shared memory array is used for counting elements greater/lower than pivot and for bitonic sort.
        // max(intra-block scan array size, array size for bitonic sort ("2 *" because of key-value pairs))
        uint_t sharedMemSize = max(
            2 * threadsSortLocal * sizeof(uint_t), (sortingKeyOnly ? 1 : 2) * thresholdBitonicSort * sizeof(*d_keys)
        );
        dim3 dimGrid(numThreadBlocks, 1, 1);
        dim3 dimBlock(threadsSortLocal, 1, 1);

        error = cudaMemcpy(d_localSeq, h_localSeq, numThreadBlocks * sizeof(*d_localSeq), cudaMemcpyHostToDevice);
        checkCudaError(error);

        if (sortingKeyOnly)
        {
            quickSortLocalKernel
                <threadsSortLocalKo, thresholdBitonicSortKo, sortOrder><<<dimGrid, dimBlock, sharedMemSize>>>(
                d_keys, d_keysBuffer, d_localSeq
            );
        }
        else
        {
            quickSortLocalKernel
                <threadsSortLocalKv, thresholdBitonicSortKv, sortOrder><<<dimGrid, dimBlock, sharedMemSize>>>(
                d_keys, d_values, d_keysBuffer, d_valuesBuffer, d_valuesPivot, d_localSeq
            );
        }
    }

    /*
    Executes parallel quicksort. Returns true, if input distribution is zero.
    */
    template <order_t sortOrder, bool sortingKeyOnly>
    bool quicksortParallel(
        data_t *h_keys, data_t *d_keys, data_t *d_values, data_t *d_keysBuffer, data_t *d_valuesBuffer,
        data_t *d_valuesPivot, data_t *h_minMaxValues, h_glob_seq_t *h_globalSeqHost,
        h_glob_seq_t *h_globalSeqHostBuffer, d_glob_seq_t *h_globalSeqDev, d_glob_seq_t *d_globalSeqDev,
        uint_t *h_globalSeqIndexes, uint_t *d_globalSeqIndexes, loc_seq_t *h_localSeq, loc_seq_t *d_localSeq,
        uint_t arrayLength
    )
    {
        uint_t thresholdPartitionGlobal = sortingKeyOnly ? threasholdPartitionGlobalKo : threasholdPartitionGlobalKv;
        uint_t threadsSortGlobal = sortingKeyOnly ? threadsSortGlobalKo : threadsSortGlobalKv;
        uint_t elemsSortGlobal = sortingKeyOnly ? elemsSortGlobalKo : elemsSortGlobalKv;

        uint_t numSeqGlobal = 1; // Number of sequences for GLOBAL quicksort
        uint_t numSeqLocal = 0;  // Number of sequences for LOCAL quicksort
        uint_t numSeqLimit = (arrayLength - 1) / thresholdPartitionGlobal + 1;
        uint_t elemsPerThreadBlock = threadsSortGlobal * elemsSortGlobal;
        bool generateSequences = arrayLength > thresholdPartitionGlobal;
        data_t minVal, maxVal;

        // Searches for min and max value in input array
        minMaxReduction(h_keys, d_keys, d_keysBuffer, h_minMaxValues, arrayLength, minVal, maxVal);
        // Null/zero distribution
        if (minVal == maxVal)
        {
            return true;
        }
        h_globalSeqHost[0].setInitSeq(arrayLength, minVal, maxVal);

        // GLOBAL QUICKSORT
        while (generateSequences)
        {
            uint_t threadBlockCounter = 0;

            // Transfers host sequences to device sequences (device needs different data about sequence than host)
            for (uint_t seqIdx = 0; seqIdx < numSeqGlobal; seqIdx++)
            {
                uint_t threadBlocksPerSeq = (h_globalSeqHost[seqIdx].length - 1) / elemsPerThreadBlock + 1;
                h_globalSeqDev[seqIdx].setFromHostSeq(
                    h_globalSeqHost[seqIdx], threadBlockCounter, threadBlocksPerSeq
                );

                // For all thread blocks in current iteration marks, they are assigned to current sequence.
                for (uint_t blockIdx = 0; blockIdx < threadBlocksPerSeq; blockIdx++)
                {
                    h_globalSeqIndexes[threadBlockCounter++] = seqIdx;
                }
            }

            runQuickSortGlobalKernel<sortOrder, sortingKeyOnly>(
                d_keys, d_values, d_keysBuffer, d_valuesBuffer, d_valuesPivot, h_globalSeqDev, d_globalSeqDev,
                h_globalSeqIndexes, d_globalSeqIndexes, numSeqGlobal, threadBlockCounter
            );

            uint_t numSeqGlobalOld = numSeqGlobal;
            numSeqGlobal = 0;

            // Generates new sub-sequences and depending on their size adds them to list for GLOBAL or LOCAL quicksort
            // If theoretical number of sequences reached limit, sequences are transferred to array for LOCAL quicksort
            for (uint_t seqIdx = 0; seqIdx < numSeqGlobalOld; seqIdx++)
            {
                h_glob_seq_t seqHost = h_globalSeqHost[seqIdx];
                d_glob_seq_t seqDev = h_globalSeqDev[seqIdx];

                // New subsequence (lower)
                if (seqDev.offsetLower > thresholdPartitionGlobal && numSeqGlobal < numSeqLimit)
                {
                    h_globalSeqHostBuffer[numSeqGlobal++].setLowerSeq(seqHost, seqDev);
                }
                else if (seqDev.offsetLower > 0)
                {
                    h_localSeq[numSeqLocal++].setLowerSeq(seqHost, seqDev);
                }

                // New subsequence (greater)
                if (seqDev.offsetGreater > thresholdPartitionGlobal && numSeqGlobal < numSeqLimit)
                {
                    h_globalSeqHostBuffer[numSeqGlobal++].setGreaterSeq(seqHost, seqDev);
                }
                else if (seqDev.offsetGreater > 0)
                {
                    h_localSeq[numSeqLocal++].setGreaterSeq(seqHost, seqDev);
                }
            }

            h_glob_seq_t *temp = h_globalSeqHost;
            h_globalSeqHost = h_globalSeqHostBuffer;
            h_globalSeqHostBuffer = temp;

            generateSequences &= numSeqGlobal < numSeqLimit && numSeqGlobal > 0;
        }

        // If global quicksort was not used, than sequence is initialized for LOCAL quicksort
        if (arrayLength <= thresholdPartitionGlobal)
        {
            numSeqLocal++;
            h_localSeq[0].setInitSeq(arrayLength);
        }

        runQuickSortLocalKernel<sortOrder, sortingKeyOnly>(
            d_keys, d_values, d_keysBuffer, d_valuesBuffer, d_valuesPivot, h_localSeq, d_localSeq, numSeqLocal
        );

        return false;
    }

    /*
    Wrapper for quicksort method.
    The code runs faster if arguments are passed to method. If members are accessed directly, code runs slower.
    */
    void sortKeyOnly()
    {
        if (_sortOrder == ORDER_ASC)
        {
            _isDistributionZero = quicksortParallel<ORDER_ASC, true>(
                _h_keys, _d_keys, NULL, _d_keysBuffer, NULL, NULL, _h_minMaxValues, _h_globalSeqHost,
                _h_globalSeqHostBuffer, _h_globalSeqDev, _d_globalSeqDev, _h_globalSeqIndexes, _d_globalSeqIndexes,
                _h_localSeq, _d_localSeq, _arrayLength
            );
        }
        else
        {
            _isDistributionZero = quicksortParallel<ORDER_DESC, true>(
                _h_keys, _d_keys, NULL, _d_keysBuffer, NULL, NULL, _h_minMaxValues, _h_globalSeqHost,
                _h_globalSeqHostBuffer, _h_globalSeqDev, _d_globalSeqDev, _h_globalSeqIndexes, _d_globalSeqIndexes,
                _h_localSeq, _d_localSeq, _arrayLength
            );
        }
    }

    /*
    Wrapper for quicksort method.
    The code runs faster if arguments are passed to method. if members are accessed directly, code runs slower.
    */
    void sortKeyValue()
    {
        if (_sortOrder == ORDER_ASC)
        {
            _isDistributionZero = quicksortParallel<ORDER_ASC, false>(
                _h_keys, _d_keys, _d_values, _d_keysBuffer, _d_valuesBuffer, _d_valuesPivot, _h_minMaxValues,
                _h_globalSeqHost, _h_globalSeqHostBuffer, _h_globalSeqDev, _d_globalSeqDev, _h_globalSeqIndexes,
                _d_globalSeqIndexes, _h_localSeq, _d_localSeq, _arrayLength
            );
        }
        else
        {
            _isDistributionZero = quicksortParallel<ORDER_DESC, false>(
                _h_keys, _d_keys, _d_values, _d_keysBuffer, _d_valuesBuffer, _d_valuesPivot, _h_minMaxValues,
                _h_globalSeqHost, _h_globalSeqHostBuffer, _h_globalSeqDev, _d_globalSeqDev, _h_globalSeqIndexes,
                _d_globalSeqIndexes, _h_localSeq, _d_localSeq, _arrayLength
            );
        }
    }

public:
    std::string getSortName()
    {
        return this->_sortName;
    }

    void memoryDestroy()
    {
        if (_arrayLength == 0)
        {
            return;
        }

        SortParallel::memoryDestroy();

        cudaError_t error;

        /* HOST MEMORY */

        free(_h_globalSeqHost);
        free(_h_globalSeqHostBuffer);

        // These arrays are allocated in CUDA pinned memory
        error = cudaFreeHost(_h_minMaxValues);
        checkCudaError(error);
        error = cudaFreeHost(_h_globalSeqDev);
        checkCudaError(error);
        error = cudaFreeHost(_h_globalSeqIndexes);
        checkCudaError(error);
        error = cudaFreeHost(_h_localSeq);
        checkCudaError(error);

        /* DEVICE MEMORY */

        error = cudaFree(_d_keysBuffer);
        checkCudaError(error);
        error = cudaFree(_d_valuesBuffer);
        checkCudaError(error);
        error = cudaFree(_d_valuesPivot);
        checkCudaError(error);

        error = cudaFree(_d_globalSeqDev);
        checkCudaError(error);
        error = cudaFree(_d_globalSeqIndexes);
        checkCudaError(error);
        error = cudaFree(_d_localSeq);
        checkCudaError(error);
    }
};


/*
Class for parallel quicksort.
Constant if min/max reduction is used is not passed to class, because preprocessor directives can't be used
with c++ templates.
*/
class QuicksortParallel : public QuicksortParallelBase<
    THRESHOLD_PARALLEL_REDUCTION,
    THREADS_REDUCTION, ELEMENTS_REDUCTION,
    THRESHOLD_PARTITION_SIZE_GLOBAL_KO, THRESHOLD_PARTITION_SIZE_GLOBAL_KV,
    THREADS_SORT_GLOBAL_KO, ELEMENTS_GLOBAL_KO,
    THREADS_SORT_GLOBAL_KV, ELEMENTS_GLOBAL_KV,
    THRESHOLD_BITONIC_SORT_KO, THRESHOLD_BITONIC_SORT_KV,
    THREADS_SORT_LOCAL_KO, THREADS_SORT_LOCAL_KV
>
{};

#endif
