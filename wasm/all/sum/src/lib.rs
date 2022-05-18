use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn sum(sumlen: u64, val: u32) -> u64 {
    sumlen + val as u64 + 4294967296
}
