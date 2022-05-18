use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn sum(sumlen: (u32, u32), val: u32) -> (u32, u32) {
    (sumlen.0 + val, sumlen.1 + 1)
}
