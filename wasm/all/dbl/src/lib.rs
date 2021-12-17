use scylla_bindgen::scylla_bindgen;

#[scylla_bindgen]
fn dbl(s: String) -> String {
    let mut newstr = String::new();
    newstr.push_str(&s);
    newstr.push_str(&s);
    newstr
}
