use std::collections::{BTreeSet, BTreeMap};
extern "C" {
    fn malloc(size: usize) -> usize;
    fn free(ptr: *mut usize);
}
#[no_mangle]
pub unsafe extern "C" fn _scylla_malloc(size: usize) -> u32 {
    malloc(size) as u32
}

#[no_mangle]
pub unsafe extern "C" fn _scylla_free(ptr: *mut usize) {
    free(ptr)
}

fn size_ptr(size: u32, ptr: u32) -> u64 {
    ((size as u64) << 32) + ptr as u64
}

pub trait FromCQL {
    fn from_cql(sizeptr: u64) -> Self where Self: Sized {
        let ptr = (sizeptr & 0xffffffff) as *mut usize;
        let ret = Self::deserialize_cql(sizeptr);
        unsafe { _scylla_free(ptr) };
        ret
    }
    fn deserialize_cql(sizeptr: u64) -> Self;
}

pub trait IntoCQL {
    fn into_cql(&self) -> u64 {
        let siz = self.size_cql();
        unsafe {
            let dest = _scylla_malloc(siz);
            let mut dest_slice = std::slice::from_raw_parts_mut(dest as *mut u8, siz);
            self.serialize_cql(&mut dest_slice);
            size_ptr(siz as u32, dest)
        }
    }
    fn size_cql(&self) -> usize;
    fn serialize_cql(&self, dest: &mut [u8]);
}

impl FromCQL for String {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as usize;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let vec : Vec<u8> = unsafe { std::slice::from_raw_parts(ptr, size).to_vec() };
        match String::from_utf8(vec) {
            Ok(v) => {v},
            Err(_e) => {panic!("string is not utf8 encoded")},
        }
    }
}

impl IntoCQL for String {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest.copy_from_slice(self.as_bytes());
    }
    fn size_cql(&self) -> usize {
        self.len()
    }
}

impl<T: FromCQL> FromCQL for Vec<T> {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as usize;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let mut vec = Vec::new();
        let length = u32::from_be(unsafe {*(ptr as *const u32)});
        let mut curr_ptr = (ptr as u32) + 4;
        for _ in 0..length {
            let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
            curr_ptr += 4;
            vec.push(T::deserialize_cql(size_ptr(siz, curr_ptr)));
            curr_ptr += siz;
        }
        if curr_ptr != (ptr as u32) + size as u32 {
            panic!("data has different size than specified")
        }
        vec
    }
}

impl<T: IntoCQL> IntoCQL for Vec<T> {
    fn serialize_cql(&self, dest: &mut [u8]) {
        let length = self.len() as u32;
        dest[0..4].copy_from_slice(&length.to_be_bytes());
        let mut offset = 4;
        for it in self.iter() {
            let siz = it.size_cql() as u32;
            dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
            offset += 4;
            it.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
            offset += siz as usize;
        }
    }
    fn size_cql(&self) -> usize {
        let mut ret : usize = 4;
        for it in self.iter() {
            ret += 4 + it.size_cql();
        }
        ret
    }
}

impl<T: FromCQL, U: FromCQL> FromCQL for (T, U) {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as u32;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let mut curr_ptr = ptr as u32;
        let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
        curr_ptr += 4;
        let t = T::deserialize_cql(size_ptr(siz, curr_ptr));
        curr_ptr += siz;
        let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
        curr_ptr += 4;
        let u = U::deserialize_cql(size_ptr(siz, curr_ptr));
        curr_ptr += siz;
        if curr_ptr != (ptr as u32) + size {
            panic!("data has different size than specified")
        }
        (t, u)
    }
}

impl<T: IntoCQL, U: IntoCQL> IntoCQL for (T, U) {
    fn serialize_cql(&self, dest: &mut [u8]) {
        let mut offset = 0;
        let siz0 = self.0.size_cql() as u32;
        dest[offset..(offset+4)].copy_from_slice(&siz0.to_be_bytes());
        offset += 4;
        self.0.serialize_cql(&mut dest[offset..(offset+(siz0 as usize))]);
        offset += siz0 as usize;

        let siz1 = self.1.size_cql() as u32;
        dest[offset..(offset+4)].copy_from_slice(&siz1.to_be_bytes());
        offset += 4;
        self.1.serialize_cql(&mut dest[offset..(offset+(siz1 as usize))]);
    }
    fn size_cql(&self) -> usize {
        8 + self.0.size_cql() + self.1.size_cql()
    }
}
impl<T: FromCQL, U: FromCQL, V: FromCQL> FromCQL for (T, U, V) {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as u32;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let mut curr_ptr = ptr as u32;
        let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
        curr_ptr += 4;
        let t = T::deserialize_cql(size_ptr(siz, curr_ptr));
        curr_ptr += siz;
        let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
        curr_ptr += 4;
        let u = U::deserialize_cql(size_ptr(siz, curr_ptr));
        curr_ptr += siz;
        let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
        curr_ptr += 4;
        let v = V::deserialize_cql(size_ptr(siz, curr_ptr));
        curr_ptr += siz;
        if curr_ptr != (ptr as u32) + size {
            panic!("data has different size than specified")
        }
        (t, u, v)
    }
}

impl<T: IntoCQL, U: IntoCQL, V: IntoCQL> IntoCQL for (T, U, V) {
    fn serialize_cql(&self, dest: &mut [u8]) {
        let mut offset = 0;
        let siz0 = self.0.size_cql() as u32;
        dest[offset..(offset+4)].copy_from_slice(&siz0.to_be_bytes());
        offset += 4;
        self.0.serialize_cql(&mut dest[offset..(offset+(siz0 as usize))]);
        offset += siz0 as usize;

        let siz1 = self.1.size_cql() as u32;
        dest[offset..(offset+4)].copy_from_slice(&siz1.to_be_bytes());
        offset += 4;
        self.1.serialize_cql(&mut dest[offset..(offset+(siz1 as usize))]);
        offset += siz1 as usize;

        let siz2 = self.2.size_cql() as u32;
        dest[offset..(offset+4)].copy_from_slice(&siz2.to_be_bytes());
        offset += 4;
        self.2.serialize_cql(&mut dest[offset..(offset+(siz2 as usize))]);
    }
    fn size_cql(&self) -> usize {
        12 + self.0.size_cql() + self.1.size_cql() + self.2.size_cql()
    }
}

impl<K: FromCQL + std::cmp::Ord, V: FromCQL> FromCQL for BTreeMap<K,V> {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as usize;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let mut map = BTreeMap::new();
        let length = u32::from_be(unsafe {*(ptr as *const u32)});
        let mut curr_ptr = (ptr as u32) + 4;
        for _ in 0..length {
            let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
            curr_ptr += 4;
            let key = K::deserialize_cql(size_ptr(siz, curr_ptr));
            curr_ptr += siz;
            let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
            curr_ptr += 4;
            let val = V::deserialize_cql(size_ptr(siz, curr_ptr));
            curr_ptr += siz;
            map.insert(key, val);
        }
        if curr_ptr != (ptr as u32) + size as u32 {
            panic!("data has different size than specified")
        }
        map
    }
}
impl<T: FromCQL + std::cmp::Ord> FromCQL for BTreeSet<T> {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as usize;
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let mut vec = BTreeSet::new();
        let length = u32::from_be(unsafe {*(ptr as *const u32)});
        let mut curr_ptr = (ptr as u32) + 4;
        for _ in 0..length {
            let siz = u32::from_be(unsafe {*(curr_ptr as *const u32)});
            curr_ptr += 4;
            vec.insert(T::deserialize_cql(size_ptr(siz, curr_ptr)));
            curr_ptr += siz;
        }
        if curr_ptr != (ptr as u32) + size as u32 {
            panic!("data has different size than specified")
        }
        vec
    }
}

impl<T: IntoCQL + std::cmp::Ord> IntoCQL for BTreeSet<T> {
    fn serialize_cql(&self, dest: &mut [u8]) {
        let length = self.len() as u32;
        dest[0..4].copy_from_slice(&length.to_be_bytes());
        let mut offset = 4;
        for it in self.iter() {
            let siz = it.size_cql() as u32;
            dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
            offset += 4;
            it.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
            offset += siz as usize;
        }
    }
    fn size_cql(&self) -> usize {
        let mut ret : usize = 4;
        for it in self.iter() {
            ret += 4 + it.size_cql();
        }
        ret
    }
}


impl<K: IntoCQL + std::cmp::Ord, V: IntoCQL> IntoCQL for BTreeMap<K,V> {
    fn serialize_cql(&self, dest: &mut [u8]) {
        let length = self.len() as u32;
        dest[0..4].copy_from_slice(&length.to_be_bytes());
        let mut offset = 4;
        for it in self.iter() {
            let siz = it.0.size_cql() as u32;
            dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
            offset += 4;
            it.0.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
            offset += siz as usize;
            let siz = it.1.size_cql() as u32;
            dest[offset..(offset+4)].copy_from_slice(&siz.to_be_bytes());
            offset += 4;
            it.1.serialize_cql(&mut dest[offset..(offset+(siz as usize))]);
            offset += siz as usize;
        }
    }
    fn size_cql(&self) -> usize {
        let mut ret : usize = 4;
        for it in self.iter() {
            ret += 4 + it.0.size_cql();
            ret += 4 + it.1.size_cql();
        }
        ret
    }
}

impl<T: FromCQL> FromCQL for Option<T> {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let size = (sizeptr >> 32) as i32;
        if (size) == -1 {
            None
        } else {
            Some(T::deserialize_cql(sizeptr))
        }
    }
}

impl<T: IntoCQL> IntoCQL for Option<T> {
    fn into_cql(&self) -> u64 {
        match &*self {
            None => {size_ptr(1_u32.wrapping_neg(), 0)},
            Some(v) => {v.into_cql()},
        }
    }
    fn serialize_cql(&self, dest: &mut [u8]) {
        match &*self {
            None => {return;},
            Some(v) => {v.serialize_cql(dest)},
        }
    }
    fn size_cql(&self) -> usize {
        match &*self {
            None => {0},
            Some(v) => {v.size_cql()},
        }
    }
}

impl FromCQL for i8 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut i8;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        slice[0]
    }
}
impl FromCQL for i16 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut i16;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        i16::from_be(slice[0])
    }
}
impl FromCQL for i32 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut i32;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        i32::from_be(slice[0])
    }
}
impl FromCQL for i64 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut i64;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        i64::from_be(slice[0])
    }
}
impl FromCQL for u8 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut u8;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        slice[0]
    }
}
impl FromCQL for u16 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut u16;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        u16::from_be(slice[0])
    }
}
impl FromCQL for u32 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut u32;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        u32::from_be(slice[0])
    }
}
impl FromCQL for u64 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut u64;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        u64::from_be(slice[0])
    }
}
impl FromCQL for f32 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut f32;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        f32::from_be_bytes(slice[0].to_bits().to_ne_bytes())
    }
}
impl FromCQL for f64 {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut f64;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        f64::from_be_bytes(slice[0].to_bits().to_ne_bytes())
    }
}
impl FromCQL for bool {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut bool;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        slice[0]
    }
}
impl FromCQL for char {
    fn deserialize_cql(sizeptr: u64) -> Self {
        let ptr = (sizeptr & 0xffffffff) as *mut char;
        let slice = unsafe { std::slice::from_raw_parts(ptr, 1) };
        slice[0]
    }
}


impl IntoCQL for i8 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        1
    }
}
impl IntoCQL for i16 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        2
    }
}
impl IntoCQL for i32 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        4
    }
}
impl IntoCQL for i64 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        8
    }
}
impl IntoCQL for u8 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        1
    }
}
impl IntoCQL for u16 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        2
    }
}
impl IntoCQL for u32 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        4
    }
}
impl IntoCQL for u64 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        8
    }
}
impl IntoCQL for f32 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        4
    }
}
impl IntoCQL for f64 {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&self.to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        8
    }
}
impl IntoCQL for bool {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&(*self as u8).to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        1
    }
}
impl IntoCQL for char {
    fn serialize_cql(&self, dest: &mut [u8]) {
        dest[0..self.size_cql()].copy_from_slice(&(*self as u8).to_be_bytes());
    }
    fn size_cql(&self) -> usize {
        1
    }
}
