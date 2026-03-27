-- Seed enrolled_photos for existing enrolled people
-- Run this once per person when photos are uploaded to Storage manually
-- (New enrollments via the admin panel do this automatically)
--
-- Person UUIDs (from enrolled_people table):
--   Nedas:   1b7cf4de-62dd-428f-ac62-5bb92277a639
--   Natania: 2e5d10c9-a71c-4975-b66c-d081deab29e4
--   Anthony: 98d179b1-0fc0-459f-80a4-9b3c21885c15
--
-- Storage paths must match the actual file paths inside the enrolled-photos bucket

insert into enrolled_photos (person_id, storage_path) values
  -- Nedas
  ('1b7cf4de-62dd-428f-ac62-5bb92277a639', 'Nedas/Nedas1.jpg'),
  ('1b7cf4de-62dd-428f-ac62-5bb92277a639', 'Nedas/Nedas2.jpg'),
  ('1b7cf4de-62dd-428f-ac62-5bb92277a639', 'Nedas/Nedas3.jpg'),
  ('1b7cf4de-62dd-428f-ac62-5bb92277a639', 'Nedas/Nedas4.jpg'),

  -- Natania
  ('2e5d10c9-a71c-4975-b66c-d081deab29e4', 'Natania/Natania1.jpg'),
  ('2e5d10c9-a71c-4975-b66c-d081deab29e4', 'Natania/Natania2.jpg'),
  ('2e5d10c9-a71c-4975-b66c-d081deab29e4', 'Natania/Natania3.jpg'),

  -- Anthony
  ('98d179b1-0fc0-459f-80a4-9b3c21885c15', 'Anthony/Anthony1.jpg'),
  ('98d179b1-0fc0-459f-80a4-9b3c21885c15', 'Anthony/Anthony2.jpg'),
  ('98d179b1-0fc0-459f-80a4-9b3c21885c15', 'Anthony/Anthony3.jpg');

-- To add more photos for an existing person manually:
-- insert into enrolled_photos (person_id, storage_path) values
--   ('<person-uuid>', '<PersonName>/<PersonName>N.jpg');
--
-- To add a completely new person manually:
-- insert into enrolled_people (name, description) values ('NewName', 'optional description');
-- then run select id from enrolled_people where name = 'NewName'; to get their UUID
-- then insert into enrolled_photos as above
