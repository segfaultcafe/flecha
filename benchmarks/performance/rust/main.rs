use std::env;

const ROUND_COUNT: usize = 10_000_000;
const COLLECTION_COUNT: usize = 4_000_000;
const COLLECTION_GROWTH_COUNT: usize = 1_000_000;
const UTF8_GROUPS: usize = 8_192;
const UTF8_REPETITIONS: usize = 128;
const FNV_BYTES: usize = 262_144;
const FNV_REPETITIONS: usize = 64;

fn rounds() -> u64 {
    let mut left = 81_985_529_216_486_895_u64;
    let mut right = 18_364_758_544_493_064_720_u64;
    for index in 0..ROUND_COUNT {
        left = left.rotate_left(7) ^ right;
        right = right
            .rotate_left(13)
            .wrapping_add(left)
            .wrapping_add(index as u64);
        left ^= right.rotate_left(17);
    }
    left ^ right
}

fn collections_workload() -> u64 {
    let mut values = Vec::with_capacity(COLLECTION_COUNT);
    for index in 0..COLLECTION_COUNT {
        values.push(index as u64);
    }
    values.into_iter().fold(0_u64, u64::wrapping_add)
}

fn collections_growth_workload() -> u64 {
    let mut values = Vec::new();
    for index in 0..COLLECTION_GROWTH_COUNT {
        values.push(index as u64);
    }
    values.into_iter().fold(0_u64, u64::wrapping_add)
}

fn count_valid_utf8(bytes: &[u8]) -> Option<u64> {
    let mut offset = 0_usize;
    let mut count = 0_u64;
    while offset < bytes.len() {
        let first = bytes[offset];
        let width;
        if first <= 127 {
            width = 1;
        } else if (194..=223).contains(&first) {
            width = 2;
            if bytes.len() - offset < width || !(128..=191).contains(&bytes[offset + 1]) {
                return None;
            }
        } else if (224..=239).contains(&first) {
            width = 3;
            if bytes.len() - offset < width {
                return None;
            }
            let second = bytes[offset + 1];
            if !(128..=191).contains(&second)
                || !(128..=191).contains(&bytes[offset + 2])
                || (first == 224 && second < 160)
                || (first == 237 && second > 159)
            {
                return None;
            }
        } else if (240..=244).contains(&first) {
            width = 4;
            if bytes.len() - offset < width {
                return None;
            }
            let second = bytes[offset + 1];
            if !(128..=191).contains(&second)
                || !(128..=191).contains(&bytes[offset + 2])
                || !(128..=191).contains(&bytes[offset + 3])
                || (first == 240 && second < 144)
                || (first == 244 && second > 143)
            {
                return None;
            }
        } else {
            return None;
        }
        count += 1;
        offset += width;
    }
    Some(count)
}

fn utf8_workload() -> u64 {
    let group = [65, 194, 162, 226, 130, 172, 240, 159, 152, 128];
    let mut bytes = Vec::with_capacity(UTF8_GROUPS * group.len());
    for _ in 0..UTF8_GROUPS {
        bytes.extend_from_slice(&group);
    }
    let mut total = 0_u64;
    for _ in 0..UTF8_REPETITIONS {
        total = total.wrapping_add(count_valid_utf8(&bytes).expect("valid UTF-8"));
    }
    total
}

fn fnv_workload() -> u64 {
    let mut bytes = Vec::with_capacity(FNV_BYTES);
    for index in 0..FNV_BYTES {
        bytes.push((index.wrapping_mul(31).wrapping_add(7)) as u8);
    }
    let mut hash = 14_695_981_039_346_656_037_u64;
    for _ in 0..FNV_REPETITIONS {
        for value in &bytes {
            hash = (hash ^ u64::from(*value)).wrapping_mul(1_099_511_628_211);
        }
    }
    hash
}

fn main() {
    let workload = env::args().nth(1).unwrap_or_default();
    let checksum = match workload.as_str() {
        "rounds" => rounds(),
        "collections" => collections_workload(),
        "collections_growth" => collections_growth_workload(),
        "utf8" => utf8_workload(),
        "fnv" => fnv_workload(),
        _ => std::process::exit(3),
    };
    println!("{checksum}");
}
