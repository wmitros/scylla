use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn div(sumlen: (u32, u32)) -> f32 {
    if sumlen.1 == 0 {
        return 0.0;
    }
    sumlen.0 as f32/sumlen.1 as f32
}
