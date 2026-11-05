package main

import (
	"fmt"
	"os"
)

const (
	roundCount            = 10000000
	collectionCount       = 4000000
	collectionGrowthCount = 1000000
	utf8Groups            = 8192
	utf8Repetitions       = 128
	fnvBytes              = 262144
	fnvRepetitions        = 64
)

func rotateLeft(value uint64, count uint) uint64 {
	return value<<count | value>>(64-count)
}

func rounds() uint64 {
	left := uint64(81985529216486895)
	right := uint64(18364758544493064720)
	for index := 0; index < roundCount; index++ {
		left = rotateLeft(left, 7) ^ right
		right = rotateLeft(right, 13) + left + uint64(index)
		left ^= rotateLeft(right, 17)
	}
	return left ^ right
}

func collectionsWorkload() uint64 {
	values := make([]uint64, 0, collectionCount)
	for index := 0; index < collectionCount; index++ {
		values = append(values, uint64(index))
	}
	var total uint64
	for _, value := range values {
		total += value
	}
	return total
}

func collectionsGrowthWorkload() uint64 {
	values := make([]uint64, 0)
	for index := 0; index < collectionGrowthCount; index++ {
		values = append(values, uint64(index))
	}
	var total uint64
	for _, value := range values {
		total += value
	}
	return total
}

func countValidUTF8(bytes []byte) (uint64, bool) {
	var count uint64
	for offset := 0; offset < len(bytes); {
		first := bytes[offset]
		width := 1
		if first <= 127 {
			width = 1
		} else if first >= 194 && first <= 223 {
			width = 2
			if len(bytes)-offset < width || bytes[offset+1] < 128 || bytes[offset+1] > 191 {
				return 0, false
			}
		} else if first >= 224 && first <= 239 {
			width = 3
			if len(bytes)-offset < width {
				return 0, false
			}
			second := bytes[offset+1]
			if second < 128 || second > 191 || bytes[offset+2] < 128 || bytes[offset+2] > 191 ||
				(first == 224 && second < 160) || (first == 237 && second > 159) {
				return 0, false
			}
		} else if first >= 240 && first <= 244 {
			width = 4
			if len(bytes)-offset < width {
				return 0, false
			}
			second := bytes[offset+1]
			if second < 128 || second > 191 || bytes[offset+2] < 128 || bytes[offset+2] > 191 ||
				bytes[offset+3] < 128 || bytes[offset+3] > 191 ||
				(first == 240 && second < 144) || (first == 244 && second > 143) {
				return 0, false
			}
		} else {
			return 0, false
		}
		count++
		offset += width
	}
	return count, true
}

func utf8Workload() uint64 {
	group := []byte{65, 194, 162, 226, 130, 172, 240, 159, 152, 128}
	bytes := make([]byte, 0, utf8Groups*len(group))
	for index := 0; index < utf8Groups; index++ {
		bytes = append(bytes, group...)
	}
	var total uint64
	for index := 0; index < utf8Repetitions; index++ {
		count, valid := countValidUTF8(bytes)
		if !valid {
			os.Exit(4)
		}
		total += count
	}
	return total
}

func fnvWorkload() uint64 {
	bytes := make([]byte, fnvBytes)
	for index := range bytes {
		bytes[index] = byte(index*31 + 7)
	}
	hash := uint64(14695981039346656037)
	for repetition := 0; repetition < fnvRepetitions; repetition++ {
		for _, value := range bytes {
			hash = (hash ^ uint64(value)) * 1099511628211
		}
	}
	return hash
}

func main() {
	if len(os.Args) != 2 {
		os.Exit(2)
	}
	var checksum uint64
	switch os.Args[1] {
	case "rounds":
		checksum = rounds()
	case "collections":
		checksum = collectionsWorkload()
	case "collections_growth":
		checksum = collectionsGrowthWorkload()
	case "utf8":
		checksum = utf8Workload()
	case "fnv":
		checksum = fnvWorkload()
	default:
		os.Exit(3)
	}
	fmt.Println(checksum)
}
