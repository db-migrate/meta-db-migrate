CREATE TABLE notes (id INT PRIMARY KEY, body VARCHAR(64));

-- a string with what a careless splitter would trip over
INSERT INTO notes VALUES (1, 'it''s; "quoted" fine');
