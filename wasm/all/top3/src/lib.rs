use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn top3(strings: Option<std::collections::BTreeSet<String>>, topn: Option<u16>) -> Vec<String> {
    match strings {
        None => [String::from("null")].to_vec(),
        Some(actuals) => match topn {
            None => [String::from("null2")].to_vec(),
            Some(n) => actuals.into_iter().take(n as usize).collect(),
        },
    }
}
