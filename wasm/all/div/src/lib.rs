use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn div(sumlen: u64) -> f32 {
    if sumlen < 4294967296 {
        return 0.0;
    }
    (sumlen & 0xFFFFFFFF) as f32 / (sumlen >> 32) as f32
}
